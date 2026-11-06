// aa-srcd: an Android Auto *source* (the role normally played by the phone).
//
// It enumerates as an accessory device through the f_accessory gadget, completes
// the head unit handshake (version, TLS, auth, service discovery, channel open,
// AV setup) and then streams video to the car screen. Video comes either from a
// pre-encoded file (--h264) or from rendering the LVGL dashboard and encoding it
// on the fly (--live). The display mode is whatever the head unit confirmed, so
// the encoder size follows the car, not a hard-coded value.
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <poll.h>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "certs.h"
#include "framesource.h"
#ifdef AA_SRCD_HAVE_LVGL
#include "lvglrenderer.h"
#endif
#include "messenger.h"
#include "pbwire.h"
#include "transport.h"

namespace {

constexpr uint8_t CONTROL_CHANNEL = 0;

// Control channel message ids.
constexpr uint16_t MSG_VERSION_REQUEST = 0x0001;
constexpr uint16_t MSG_VERSION_RESPONSE = 0x0002;
constexpr uint16_t MSG_SSL_HANDSHAKE = 0x0003;
constexpr uint16_t MSG_AUTH_COMPLETE = 0x0004;
constexpr uint16_t MSG_SERVICE_DISCOVERY_REQUEST = 0x0005;
constexpr uint16_t MSG_SERVICE_DISCOVERY_RESPONSE = 0x0006;
constexpr uint16_t MSG_CHANNEL_OPEN_REQUEST = 0x0007;
constexpr uint16_t MSG_CHANNEL_OPEN_RESPONSE = 0x0008;
constexpr uint16_t MSG_PING_REQUEST = 0x000b;
constexpr uint16_t MSG_PING_RESPONSE = 0x000c;
constexpr uint16_t MSG_SHUTDOWN_REQUEST = 0x000f;
constexpr uint16_t MSG_SHUTDOWN_RESPONSE = 0x0010;

// AV channel message ids (same on every AV channel).
constexpr uint16_t AV_SETUP_REQUEST = 0x8000;
constexpr uint16_t AV_START_INDICATION = 0x8001;
constexpr uint16_t AV_STOP_INDICATION = 0x8002;
constexpr uint16_t AV_SETUP_RESPONSE = 0x8003;
constexpr uint16_t AV_MEDIA_ACK_INDICATION = 0x8004;
constexpr uint16_t AV_VIDEO_FOCUS_REQUEST = 0x8007;
constexpr uint16_t AV_VIDEO_FOCUS_INDICATION = 0x8008;
constexpr uint16_t AV_MEDIA_WITH_TIMESTAMP_INDICATION = 0x0000;
constexpr uint16_t AV_MEDIA_INDICATION = 0x0001;

// Sensor channel message ids.
constexpr uint16_t SENSOR_START_REQUEST = 0x8001;      // actually SensorRequest
constexpr uint16_t SENSOR_START_RESPONSE = 0x8002;
constexpr uint16_t SENSOR_EVENT_INDICATION = 0x8003;

// Input channel message ids.
constexpr uint16_t INPUT_EVENT_INDICATION = 0x8001;

constexpr int MEDIA_CODEC_H264_BP = 3;
constexpr int AV_SETUP_STATUS_OK = 2;
constexpr int STATUS_OK = 0;
constexpr int STATUS_FAIL = 1;
constexpr int VIDEO_FOCUS_PROJECTED = 1;

enum class State {
    WaitingHandshake,
    Tls,
    Authenticated,
    Discovered,
    Streaming,
    Stopped,
};

struct Options {
    bool useUsb = true;
    uint16_t tcpPort = 5288;
    std::string h264Path;
    bool live = false;
    int width = 800;
    int height = 480;
    int bitrateKbps = 1200;
    int fps = 5;
    bool timestampedMedia = false;
    size_t maxFramePayload = 32768;
    bool debug = false;
    std::string ui = "auto";         // auto | lvgl | pattern
    std::string deviceName = "Android";
    std::string deviceBrand = "aa-srcd";
};

// One display mode the head unit said it can decode. The position inside the
// advertised list is what AVChannelStartIndication's config field refers to.
struct VideoMode {
    int resolutionEnum = 0;
    int fpsEnum = 0;
    int width = 0;
    int height = 0;
    int fps = 0;
    int dpi = 0;
    int marginWidth = 0;
    int marginHeight = 0;
};

struct ChannelInfo {
    int channelId = -1;
    bool hasVideoConfigs = false;
    bool hasAudioConfigs = false;
    bool hasInput = false;
    bool hasSensor = false;
    std::vector<int> sensorTypes;
    std::vector<VideoMode> videoModes;
};

// VideoResolution enum as documented by open-android-auto (values 5-9 were wrong
// in the aasdk-era definitions).
bool videoModeSize(int resolutionEnum, int& width, int& height) {
    switch (resolutionEnum) {
        case 1: width = 800;   height = 480;   return true;
        case 2: width = 1280;  height = 720;   return true;
        case 3: width = 1920;  height = 1080;  return true;
        case 4: width = 2560;  height = 1440;  return true;
        case 5: width = 3840;  height = 2160;  return true;
        case 6: width = 720;   height = 1280;  return true;
        case 7: width = 1080;  height = 1920;  return true;
        case 8: width = 1440;  height = 2560;  return true;
        case 9: width = 2160;  height = 3840;  return true;
        default: return false;
    }
}

int videoFpsFromEnum(int fpsEnum) {
    switch (fpsEnum) {
        case 1: return 60;
        case 2: return 30;
        default: return 0;     // unknown: do not cap our own rate
    }
}

uint64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void appendBe16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back((uint8_t)((value >> 8) & 0xff));
    out.push_back((uint8_t)(value & 0xff));
}

void appendBe64(std::vector<uint8_t>& out, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back((uint8_t)((value >> shift) & 0xff));
    }
}

class Source {
public:
    explicit Source(Options options): m_options(std::move(options)) {}

    int run() {
        if (!m_cryptor.initServer(kCertificatePem, kPrivateKeyPem)) {
            return 1;
        }

        Transport* transport;
        UsbAccessoryTransport usbTransport;
        TcpServerTransport tcpTransport(m_options.tcpPort);

        if (m_options.useUsb) {
            transport = &usbTransport;
        }
        else {
            transport = &tcpTransport;
        }

        if (!transport->open()) {
            return 1;
        }

        Messenger messenger(*transport, m_cryptor);
        messenger.setMaxFramePayload(m_options.maxFramePayload);

        if (m_options.live) {
            if (!aaSrcdHasX264Support()) {
                fprintf(stderr, "this build has no x264 support, cannot render live\n");
                return 1;
            }
            // The encoder size depends on the display mode the head unit picks, so
            // the live source waits for the AV setup response.
            m_streamFps = m_options.fps;
        }
        else {
            m_source = std::make_unique<H264FileSource>(m_options.h264Path);
            if (!m_source || !m_source->isReady()) {
                fprintf(stderr, "no video to send: use --h264 <file> or --live\n");
                return 1;
            }
            fprintf(stderr, "video source ready: %s\n", m_source->name());
        }

        m_dashboard.sessionStartMs = nowMs();
        m_nextFrameMs = nowMs();

        struct pollfd pfd;
        pfd.fd = transport->fd();
        pfd.events = POLLIN;
        pfd.revents = 0;
        m_nextPingMs = nowMs() + 5000;

        while (m_state != State::Stopped) {
            int ready = poll(&pfd, 1, 100);
            if (ready < 0 && errno != EINTR) {
                break;
            }

            if (ready > 0 && !messenger.pump()) {
                fprintf(stderr, "connection closed by the head unit\n");
                break;
            }

            Message message;
            while (messenger.nextMessage(message)) {
                handleMessage(messenger, message);
            }

            pumpVideo(messenger);
            keepAlive(messenger);
        }

        fprintf(stderr, "session finished: %llu frames sent\n", (unsigned long long)m_framesSent);
        return 0;
    }

private:
    bool sendMessage(Messenger& messenger, uint8_t channel, bool encrypted, bool control, uint16_t messageId, const pb::Writer& body = pb::Writer()) {
        Message message;
        message.channel = channel;
        message.encrypted = encrypted;
        message.control = control;
        appendBe16(message.payload, messageId);
        message.payload.insert(message.payload.end(), body.data().begin(), body.data().end());

        return messenger.send(std::move(message));
    }

    void handleMessage(Messenger& messenger, const Message& message) {
        if (m_options.debug) {
            fprintf(stderr, "recv ch=%u type=%s enc=%d id=0x%04x len=%zu\n", message.channel,
                message.control ? "control" : "specific", message.encrypted ? 1 : 0, message.messageId(), message.payload.size());
        }

        if (message.channel == CONTROL_CHANNEL) {
            handleControl(messenger, message);
            return;
        }

        if (message.channel == (uint8_t)m_videoChannelId) {
            handleVideo(messenger, message);
            return;
        }

        if (message.channel == (uint8_t)m_sensorChannelId) {
            handleSensor(message);
            return;
        }

        if (message.channel == (uint8_t)m_inputChannelId && message.messageId() == INPUT_EVENT_INDICATION) {
            return; // static dashboard: input is not used yet
        }

        if (m_options.debug) {
            fprintf(stderr, "ignoring ch=%u id=0x%04x\n", message.channel, message.messageId());
        }
    }

    void handleControl(Messenger& messenger, const Message& message) {
        switch (message.messageId()) {
            case MSG_VERSION_REQUEST: {
                // payload: id(2) major(2) minor(2), raw binary, not protobuf.
                int requestedMinor = message.payload.size() >= 6 ? (message.payload[4] << 8) | message.payload[5] : 1;

                std::vector<uint8_t> body;
                appendBe16(body, 1);                 // major
                appendBe16(body, 7);                 // minor
                appendBe16(body, 0);                 // MATCH
                Message reply;
                reply.channel = CONTROL_CHANNEL;
                reply.control = true;
                appendBe16(reply.payload, MSG_VERSION_RESPONSE);
                reply.payload.insert(reply.payload.end(), body.begin(), body.end());

                m_state = State::Tls;
                fprintf(stderr, "head unit asks for protocol v1.%d, answering v1.7\n", requestedMinor);
                messenger.send(std::move(reply));
                break;
            }
            case MSG_SSL_HANDSHAKE: {
                const uint8_t* in = message.payload.data() + 2;
                size_t inLen = message.payload.size() >= 2 ? message.payload.size() - 2 : 0;

                std::vector<uint8_t> out;
                if (!m_cryptor.handleHandshake(in, inLen, out)) {
                    fprintf(stderr, "TLS handshake failed\n");
                    m_state = State::Stopped;
                    return;
                }

                if (!out.empty()) {
                    Message reply;
                    reply.channel = CONTROL_CHANNEL;
                    reply.control = true;
                    appendBe16(reply.payload, MSG_SSL_HANDSHAKE);
                    reply.payload.insert(reply.payload.end(), out.begin(), out.end());
                    messenger.send(std::move(reply));
                }

                if (m_cryptor.isReady() && m_state == State::Tls) {
                    m_state = State::Authenticated;
                    fprintf(stderr, "waiting for AUTH_COMPLETE / SERVICE_DISCOVERY_RESPONSE\n");
                }
                break;
            }
            case MSG_AUTH_COMPLETE:
                fprintf(stderr, "authenticated by the head unit\n");
                break;
            case MSG_SERVICE_DISCOVERY_RESPONSE:
                parseServiceDiscovery(message.payload);
                sendServiceDiscoveryRequest(messenger);
                m_state = State::Discovered;
                fprintf(stderr, "waiting for the head unit to open channels\n");
                break;
            case MSG_CHANNEL_OPEN_REQUEST:
                handleChannelOpen(messenger, message, message.channel);
                break;
            case MSG_PING_REQUEST: {
                std::vector<pb::Field> fields;
                if (message.payload.size() > 2) {
                    pb::parse(message.payload.data() + 2, message.payload.size() - 2, fields);
                }

                pb::Writer body;
                const pb::Field* timestamp = pb::find(fields, 1);
                if (timestamp) {
                    body.addInt(1, (int64_t)timestamp->value);
                }

                sendMessage(messenger, CONTROL_CHANNEL, false, true, MSG_PING_RESPONSE, body);
                break;
            }
            case MSG_SHUTDOWN_REQUEST:
                sendMessage(messenger, CONTROL_CHANNEL, true, true, MSG_SHUTDOWN_RESPONSE);
                m_state = State::Stopped;
                break;
            default:
                if (m_options.debug) {
                    fprintf(stderr, "unhandled control message 0x%04x\n", message.messageId());
                }
                break;
        }
    }

    void parseServiceDiscovery(const std::vector<uint8_t>& payload) {
        std::vector<pb::Field> fields;
        pb::parse(payload.data() + 2, payload.size() - 2, fields);

        const pb::Field* headUnitName = pb::find(fields, 2);
        const pb::Field* carModel = pb::find(fields, 3);
        fprintf(stderr, "head unit: %s / %s\n",
            headUnitName ? headUnitName->bytes.c_str() : "?", carModel ? carModel->bytes.c_str() : "?");

        for (const pb::Field* descriptor: pb::findAll(fields, 1)) {
            std::vector<pb::Field> channelFields;
            pb::parse((const uint8_t*)descriptor->bytes.data(), descriptor->bytes.size(), channelFields);

            const pb::Field* channelIdField = pb::find(channelFields, 1);
            if (!channelIdField) {
                continue;
            }

            ChannelInfo info;
            info.channelId = (int)channelIdField->value;
            const pb::Field* sensorChannel = pb::find(channelFields, 2);
            info.hasSensor = sensorChannel != nullptr;
            if (sensorChannel) {
                std::vector<pb::Field> sensorFields;
                pb::parse((const uint8_t*)sensorChannel->bytes.data(), sensorChannel->bytes.size(), sensorFields);
                for (const pb::Field* type: pb::findAll(sensorFields, 1)) {
                    info.sensorTypes.push_back((int)type->value);
                }
            }
            info.hasInput = pb::find(channelFields, 4) != nullptr;

            const pb::Field* avChannel = pb::find(channelFields, 3);
            if (avChannel) {
                std::vector<pb::Field> avFields;
                pb::parse((const uint8_t*)avChannel->bytes.data(), avChannel->bytes.size(), avFields);

                for (const pb::Field* videoConfig: pb::findAll(avFields, 4)) {
                    std::vector<pb::Field> configFields;
                    pb::parse((const uint8_t*)videoConfig->bytes.data(), videoConfig->bytes.size(), configFields);

                    VideoMode mode;
                    const pb::Field* resolution = pb::find(configFields, 1);
                    const pb::Field* fps = pb::find(configFields, 2);
                    const pb::Field* marginWidth = pb::find(configFields, 3);
                    const pb::Field* marginHeight = pb::find(configFields, 4);
                    const pb::Field* dpi = pb::find(configFields, 5);

                    mode.resolutionEnum = resolution ? (int)resolution->value : 0;
                    mode.fpsEnum = fps ? (int)fps->value : 0;
                    mode.marginWidth = marginWidth ? (int)marginWidth->value : 0;
                    mode.marginHeight = marginHeight ? (int)marginHeight->value : 0;
                    mode.dpi = dpi ? (int)dpi->value : 0;
                    mode.fps = videoFpsFromEnum(mode.fpsEnum);
                    videoModeSize(mode.resolutionEnum, mode.width, mode.height);

                    info.hasVideoConfigs = true;
                    info.videoModes.push_back(mode);
                }

                info.hasAudioConfigs = !pb::findAll(avFields, 3).empty();
            }

            fprintf(stderr, "  channel %d:%s%s%s%s", info.channelId,
                info.hasVideoConfigs ? " video" : "", info.hasAudioConfigs ? " audio" : "",
                info.hasInput ? " input" : "", info.hasSensor ? " sensor" : "");
            for (const VideoMode& mode: info.videoModes) {
                fprintf(stderr, " [%dx%d %dfps dpi=%d margins=%dx%d]", mode.width, mode.height, mode.fps, mode.dpi,
                    mode.marginWidth, mode.marginHeight);
            }
            fprintf(stderr, "\n");

            if (info.hasVideoConfigs && m_videoChannelId < 0) {
                m_videoChannelId = info.channelId;
                m_videoModes = info.videoModes;
                selectVideoMode();
            }
            else if (info.hasSensor && m_sensorChannelId < 0) {
                m_sensorChannelId = info.channelId;
                m_sensorTypes = info.sensorTypes;
            }
            else if (info.hasInput && m_inputChannelId < 0) {
                m_inputChannelId = info.channelId;
            }
        }

        if (m_videoChannelId < 0) {
            fprintf(stderr, "the head unit advertised no video channel, cannot project anything\n");
        }
    }

    void sendServiceDiscoveryRequest(Messenger& messenger) {
        pb::Writer body;
        body.addString(4, m_options.deviceName);
        body.addString(5, m_options.deviceBrand);
        sendMessage(messenger, CONTROL_CHANNEL, true, true, MSG_SERVICE_DISCOVERY_REQUEST, body);
    }

    void handleChannelOpen(Messenger& messenger, const Message& message, uint8_t responseChannel) {
        std::vector<pb::Field> fields;
        pb::parse(message.payload.data() + 2, message.payload.size() - 2, fields);

        const pb::Field* channelIdField = pb::find(fields, 2);
        int channelId = channelIdField ? (int)channelIdField->value : -1;

        bool accept = channelId == m_videoChannelId || channelId == m_sensorChannelId || channelId == m_inputChannelId;

        pb::Writer body;
        body.addVarint(1, accept ? STATUS_OK : STATUS_FAIL);
        sendMessage(messenger, responseChannel, true, true, MSG_CHANNEL_OPEN_RESPONSE, body);

        fprintf(stderr, "channel open request for %d -> %s\n", channelId, accept ? "accepted" : "refused");

        if (accept && channelId == m_videoChannelId) {
            pb::Writer setup;
            setup.addVarint(1, MEDIA_CODEC_H264_BP);
            sendMessage(messenger, (uint8_t)m_videoChannelId, true, false, AV_SETUP_REQUEST, setup);
            fprintf(stderr, "sent AV setup request (H.264 baseline) on channel %d\n", m_videoChannelId);
        }

        if (accept && channelId == m_sensorChannelId) {
            for (int sensorType: m_sensorTypes) {
                pb::Writer request;
                request.addVarint(1, (uint64_t)sensorType);
                request.addInt(2, 1000);   // refresh interval in ms
                sendMessage(messenger, (uint8_t)m_sensorChannelId, true, false, SENSOR_START_REQUEST, request);
            }
            fprintf(stderr, "requested %zu sensor types on channel %d\n", m_sensorTypes.size(), m_sensorChannelId);
        }
    }

    // Sensors flow head unit -> source: we ask for the types the head unit
    // advertised and then read whatever it pushes.
    void handleSensor(const Message& message) {
        switch (message.messageId()) {
            case SENSOR_START_RESPONSE: {
                std::vector<pb::Field> fields;
                pb::parse(message.payload.data() + 2, message.payload.size() - 2, fields);
                const pb::Field* status = pb::find(fields, 1);
                fprintf(stderr, "sensor start response: %d\n", status ? (int)status->value : -1);
                break;
            }
            case SENSOR_EVENT_INDICATION: {
                std::vector<pb::Field> events;
                pb::parse(message.payload.data() + 2, message.payload.size() - 2, events);

                // Field numbers match SensorType values (10 = NIGHT_DATA, 13 = DRIVING_STATUS).
                for (const pb::Field* night: pb::findAll(events, 10)) {
                    std::vector<pb::Field> fields;
                    pb::parse((const uint8_t*)night->bytes.data(), night->bytes.size(), fields);
                    const pb::Field* isNight = pb::find(fields, 1);
                    bool value = isNight && isNight->value != 0;
                    if (value != m_nightMode) {
                        m_nightMode = value;
                        m_dashboard.nightMode = value;
                        fprintf(stderr, "night mode: %s\n", m_nightMode ? "night" : "day");
                    }
                }

                for (const pb::Field* driving: pb::findAll(events, 13)) {
                    std::vector<pb::Field> fields;
                    pb::parse((const uint8_t*)driving->bytes.data(), driving->bytes.size(), fields);
                    const pb::Field* status = pb::find(fields, 1);
                    if (status) {
                        m_drivingStatus = (int)status->value;
                        m_dashboard.drivingStatus = (int)status->value;
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    void handleVideo(Messenger& messenger, const Message& message) {
        switch (message.messageId()) {
            case AV_SETUP_RESPONSE: {
                std::vector<pb::Field> fields;
                pb::parse(message.payload.data() + 2, message.payload.size() - 2, fields);

                const pb::Field* status = pb::find(fields, 1);
                const pb::Field* maxUnacked = pb::find(fields, 2);
                std::vector<const pb::Field*> configs = pb::findAll(fields, 3);

                int statusValue = status ? (int)status->value : 0;
                m_maxUnacked = maxUnacked && maxUnacked->value > 0 ? (int)maxUnacked->value : 8;
                m_configIndex = !configs.empty() ? (int)configs[0]->value : m_selectedModeIndex;

                fprintf(stderr, "AV setup response: status=%d max_unacked=%d config=%d\n", statusValue, m_maxUnacked, m_configIndex);

                if (statusValue != AV_SETUP_STATUS_OK) {
                    m_state = State::Stopped;
                    return;
                }

                applyVideoMode(m_configIndex);

                if (m_options.live && !createLiveSource()) {
                    m_state = State::Stopped;
                    return;
                }

                pb::Writer start;
                start.addVarint(1, 1);                 // session id
                start.addVarint(2, (uint64_t)m_configIndex);
                sendMessage(messenger, (uint8_t)m_videoChannelId, true, false, AV_START_INDICATION, start);

                m_state = State::Streaming;
                m_nextFrameMs = nowMs();
                fprintf(stderr, "streaming started\n");
                break;
            }
            case AV_MEDIA_ACK_INDICATION: {
                std::vector<pb::Field> fields;
                pb::parse(message.payload.data() + 2, message.payload.size() - 2, fields);

                size_t acked = pb::findAll(fields, 3).size();
                if (acked == 0) {
                    acked = 1;
                }

                m_outstanding = m_outstanding > (int)acked ? m_outstanding - (int)acked : 0;
                break;
            }
            case AV_VIDEO_FOCUS_REQUEST: {
                pb::Writer focus;
                focus.addVarint(1, VIDEO_FOCUS_PROJECTED);
                sendMessage(messenger, (uint8_t)m_videoChannelId, true, false, AV_VIDEO_FOCUS_INDICATION, focus);
                fprintf(stderr, "granted video focus\n");
                break;
            }
            case AV_STOP_INDICATION:
                fprintf(stderr, "head unit stopped the video channel\n");
                m_state = State::Discovered;
                break;
            default:
                break;
        }
    }

    void pumpVideo(Messenger& messenger) {
        if (m_state != State::Streaming || !m_source) {
            return;
        }

        uint64_t now = nowMs();
        uint64_t intervalMs = 1000 / (m_streamFps > 0 ? m_streamFps : 1);

        while (m_outstanding < m_maxUnacked && now >= m_nextFrameMs) {
            std::vector<uint8_t> picture;
            bool keyframe = false;

            if (!m_source->nextPicture(picture, keyframe)) {
                m_state = State::Stopped;
                return;
            }

            if (m_options.debug) {
                fprintf(stderr, "picture %zu bytes, keyframe=%d\n", picture.size(), keyframe ? 1 : 0);
            }

            Message frame;
            frame.channel = (uint8_t)m_videoChannelId;
            frame.encrypted = true;
            frame.control = false;

            if (m_options.timestampedMedia) {
                appendBe16(frame.payload, AV_MEDIA_WITH_TIMESTAMP_INDICATION);
                appendBe64(frame.payload, nowNs());
            }
            else {
                appendBe16(frame.payload, AV_MEDIA_INDICATION);
            }
            frame.payload.insert(frame.payload.end(), picture.begin(), picture.end());

            if (!messenger.send(std::move(frame))) {
                m_state = State::Stopped;
                return;
            }

            m_outstanding++;
            m_framesSent++;
            m_nextFrameMs += intervalMs;
            now = nowMs();
        }

        if (now < m_nextFrameMs) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void keepAlive(Messenger& messenger) {
        uint64_t now = nowMs();
        if (now < m_nextPingMs) {
            return;
        }

        m_nextPingMs = now + 5000;

        pb::Writer body;
        body.addInt(1, (int64_t)now);
        sendMessage(messenger, CONTROL_CHANNEL, false, true, MSG_PING_REQUEST, body);
    }

    Options m_options;
    Cryptor m_cryptor;
    State m_state = State::WaitingHandshake;

    int m_videoChannelId = -1;
    int m_sensorChannelId = -1;
    int m_inputChannelId = -1;

    std::vector<int> m_sensorTypes;
    bool m_nightMode = false;
    int m_drivingStatus = -1;

    int m_maxUnacked = 8;
    int m_outstanding = 0;
    int m_configIndex = 0;

    std::unique_ptr<FrameSource> m_source;
    std::unique_ptr<Renderer> m_renderer;
    DashboardState m_dashboard;
    std::vector<VideoMode> m_videoModes;
    int m_selectedModeIndex = -1;
    int m_streamFps = 5;

    // Pick the advertised mode we would rather drive: an exact match with what the
    // operator asked for, otherwise the first landscape mode within reach.
    void selectVideoMode() {
        m_selectedModeIndex = -1;

        for (size_t index = 0; index < m_videoModes.size(); index++) {
            if (m_videoModes[index].width == m_options.width && m_videoModes[index].height == m_options.height) {
                m_selectedModeIndex = (int)index;
                break;
            }
        }

        if (m_selectedModeIndex < 0) {
            for (size_t index = 0; index < m_videoModes.size(); index++) {
                const VideoMode& mode = m_videoModes[index];
                if (mode.width >= mode.height && mode.width > 0 && mode.width <= 1920) {
                    m_selectedModeIndex = (int)index;
                    break;
                }
            }
        }

        if (m_selectedModeIndex < 0) {
            fprintf(stderr, "none of the %zu advertised display modes is usable by this build\n", m_videoModes.size());
        }
        else {
            const VideoMode& mode = m_videoModes[m_selectedModeIndex];
            fprintf(stderr, "selected display mode %d: %dx%d at %d fps, dpi %d\n", m_selectedModeIndex,
                mode.width, mode.height, mode.fps, mode.dpi);
        }
    }

    void applyVideoMode(int configIndex) {
        const VideoMode* mode = nullptr;
        if (configIndex >= 0 && configIndex < (int)m_videoModes.size()) {
            mode = &m_videoModes[configIndex];
        }
        else if (m_selectedModeIndex >= 0) {
            mode = &m_videoModes[m_selectedModeIndex];
        }

        if (!mode || mode->width <= 0) {
            m_dashboard.videoWidth = m_options.width;
            m_dashboard.videoHeight = m_options.height;
            m_dashboard.videoFps = m_options.fps;
            fprintf(stderr, "the head unit did not name a display mode we recognise, using %dx%d\n",
                m_dashboard.videoWidth, m_dashboard.videoHeight);
            return;
        }

        m_dashboard.videoWidth = mode->width;
        m_dashboard.videoHeight = mode->height;
        m_dashboard.videoFps = mode->fps > 0 ? std::min(m_options.fps, mode->fps) : m_options.fps;
        m_streamFps = m_dashboard.videoFps;

        fprintf(stderr, "streaming %dx%d at %d fps (mode margins %dx%d)\n", m_dashboard.videoWidth,
            m_dashboard.videoHeight, m_streamFps, mode->marginWidth, mode->marginHeight);
    }

    bool createLiveSource() {
        int width = m_dashboard.videoWidth;
        int height = m_dashboard.videoHeight;
        int fps = m_streamFps > 0 ? m_streamFps : m_options.fps;

        bool useLvgl;
#ifdef AA_SRCD_HAVE_LVGL
        useLvgl = m_options.ui != "pattern";
        if (useLvgl) {
            m_renderer = std::make_unique<LvglRenderer>(width, height);
        }
#else
        if (m_options.ui == "lvgl") {
            fprintf(stderr, "this build has no LVGL support\n");
            return false;
        }
        useLvgl = false;
#endif
        if (!useLvgl) {
            m_renderer = std::make_unique<TestPatternRenderer>();
        }

        m_source = std::make_unique<LiveEncodeSource>(width, height, fps, m_options.bitrateKbps, *m_renderer, m_dashboard);
        if (!m_source->isReady()) {
            fprintf(stderr, "cannot start the live encoder\n");
            return false;
        }

        fprintf(stderr, "live source ready: %s + %s at %dx%d\n", m_source->name(), useLvgl ? "lvgl" : "test pattern",
            width, height);
        return true;
    }
    uint64_t m_nextFrameMs = 0;
    uint64_t m_nextPingMs = 0;
    uint64_t m_framesSent = 0;
};

void printUsage(const char* argv0) {
    printf("usage: %s [options]\n"
           "  --h264 <file>       Annex-B H.264 stream to replay\n"
           "  --live              render and encode on the fly (test pattern for now)\n"
           "  --size WxH          live resolution, default 800x480\n"
           "  --bitrate <kbps>    live bitrate, default 1200\n"
           "  --ui lvgl|pattern   dashboard renderer for --live, default lvgl when built in\n"
           "  --fps <n>           playback rate, default 5\n"
           "  --tcp <port>        bench mode: wait for a head unit over TCP instead of USB\n"
           "  --timestamped       send media frames with the timestamped indication\n"
           "  --debug             log every message\n", argv0);
}

} // namespace

int main(int argc, char** argv) {
    Options options;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--h264" && i + 1 < argc) {
            options.h264Path = argv[++i];
        }
        else if (arg == "--fps" && i + 1 < argc) {
            options.fps = atoi(argv[++i]);
        }
        else if (arg == "--tcp" && i + 1 < argc) {
            options.useUsb = false;
            options.tcpPort = (uint16_t)atoi(argv[++i]);
        }
        else if (arg == "--live") {
            options.live = true;
        }
        else if (arg == "--size" && i + 1 < argc) {
            sscanf(argv[++i], "%dx%d", &options.width, &options.height);
        }
        else if (arg == "--bitrate" && i + 1 < argc) {
            options.bitrateKbps = atoi(argv[++i]);
        }
        else if (arg == "--ui" && i + 1 < argc) {
            options.ui = argv[++i];
        }
        else if (arg == "--max-frame" && i + 1 < argc) {
            options.maxFramePayload = (size_t)atoi(argv[++i]);
        }
        else if (arg == "--timestamped") {
            options.timestampedMedia = true;
        }
        else if (arg == "--debug") {
            options.debug = true;
        }
        else {
            printUsage(argv[0]);
            return 1;
        }
    }

    if (!options.live && options.h264Path.empty()) {
        printUsage(argv[0]);
        return 1;
    }

    // On USB the daemon owns the accessory node for as long as the car is
    // attached: one session ending (unplug, SHUTDOWN_REQUEST, decoder hiccup)
    // must not end the process, otherwise nothing answers the next ignition
    // cycle. Bench sessions over --tcp stay single-shot on purpose.
    int status = 0;
    do {
        Source source(options);
        status = source.run();

        if (!options.useUsb) {
            break;
        }

        fprintf(stderr, "session ended, waiting for the next head unit\n");
        sleep(1);
    } while (true);

    return status;
}
