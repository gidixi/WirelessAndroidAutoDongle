#include <cstring>
#include <algorithm>
#include <chrono>
#include <sys/stat.h>
#include <sys/types.h>

#include "common.h"
#include "frameDump.h"

namespace {
    // Flags byte of the Android Auto frame header.
    constexpr unsigned char FRAME_TYPE_FIRST = 1 << 0;
    constexpr unsigned char FRAME_TYPE_LAST = 1 << 1;
    constexpr unsigned char FRAME_TYPE_MASK = FRAME_TYPE_FIRST | FRAME_TYPE_LAST;
    constexpr unsigned char MESSAGE_TYPE_CONTROL = 1 << 2;
    constexpr unsigned char ENCRYPTION_ENCRYPTED = 1 << 3;

    uint16_t readBe16(const unsigned char* data) {
        return (uint16_t(data[0]) << 8) | data[1];
    }

    uint32_t readBe32(const unsigned char* data) {
        return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) | (uint32_t(data[2]) << 8) | data[3];
    }

    const char* frameTypeName(unsigned char frameType) {
        switch (frameType) {
            case 0: return "MIDDLE";
            case 1: return "FIRST";
            case 2: return "LAST";
            default: return "BULK";
        }
    }

    void appendHex(char* buffer, size_t bufferSize, const unsigned char* data, size_t len) {
        size_t offset = 0;
        for (size_t i = 0; i < len && offset + 3 < bufferSize; i++) {
            snprintf(buffer + offset, bufferSize - offset, "%02x", data[i]);
            offset += 2;
        }
    }
}

FrameRecorder::FrameRecorder(Direction direction, const std::string& directory):
    m_direction(direction), m_directory(directory) {

    m_name = m_direction == Direction::FROM_PHONE ? "from-phone" : "from-headunit";
    m_startMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    openFiles();
}

FrameRecorder::~FrameRecorder() {
    if (m_streamFile) {
        fclose(m_streamFile);
        m_streamFile = nullptr;
    }

    if (m_indexFile) {
        fclose(m_indexFile);
        m_indexFile = nullptr;
    }

    Logger::instance()->info("Frame capture (%s) closed\n", m_name);
}

void FrameRecorder::openFiles() {
    // Best effort: the directory may already exist, or may not be creatable.
    mkdir(m_directory.c_str(), 0755);

    std::string streamPath = m_directory + "/capture-" + m_name + ".bin";
    std::string indexPath = m_directory + "/capture-" + m_name + ".idx.csv";

    m_streamFile = fopen(streamPath.c_str(), "ab");
    m_indexFile = fopen(indexPath.c_str(), "at");

    if (!m_streamFile || !m_indexFile) {
        Logger::instance()->info("Frame capture (%s) disabled, cannot open %s or %s: %s\n",
            m_name, streamPath.c_str(), indexPath.c_str(), strerror(errno));

        if (m_streamFile) {
            fclose(m_streamFile);
            m_streamFile = nullptr;
        }
        if (m_indexFile) {
            fclose(m_indexFile);
            m_indexFile = nullptr;
        }
        return;
    }

    Logger::instance()->info("Frame capture (%s) writing %s and %s\n", m_name, streamPath.c_str(), indexPath.c_str());

    // Files are kept across connection attempts within the same boot, so frame
    // offsets must continue from whatever is already on disk.
    long existingBytes = ftell(m_streamFile);
    if (existingBytes > 0) {
        m_pendingOffset = (uint64_t) existingBytes;
    }

    fprintf(m_indexFile, "# ts_ms,dir,offset,channel,flags,frame_type,msg_type,enc,header_len,payload_len,total_msg_size,payload_hex\n");
    fprintf(m_indexFile, "# capture session start, stream continues from byte %llu\n",
        (unsigned long long) m_pendingOffset);
    fflush(m_indexFile);
}

void FrameRecorder::append(const unsigned char* data, size_t len) {
    if (!m_streamFile || !m_indexFile || len == 0) {
        return;
    }

    if (fwrite(data, 1, len, m_streamFile) != len) {
        Logger::instance()->info("Frame capture (%s) failed to write %zu bytes: %s\n", m_name, len, strerror(errno));
        return;
    }

    m_pending.insert(m_pending.end(), data, data + len);
    parsePending();

    fflush(m_streamFile);
    fflush(m_indexFile);
}

void FrameRecorder::parsePending() {
    while (true) {
        if (m_pending.size() < HEADER_LEN_SHORT) {
            break;
        }

        const unsigned char* frame = m_pending.data();
        bool fragmented = (frame[1] & FRAME_TYPE_MASK) == FRAME_TYPE_FIRST;
        size_t headerLen = fragmented ? HEADER_LEN_FRAGMENTED : HEADER_LEN_SHORT;

        if (m_pending.size() < headerLen) {
            break;
        }

        uint16_t payloadLen = readBe16(frame + 2);
        uint32_t totalMsgSize = fragmented ? readBe32(frame + 4) : 0;

        if (m_pending.size() < headerLen + payloadLen) {
            // Frame is still arriving on the wire, wait for the rest of it.
            break;
        }

        logFrame(m_pendingOffset, frame, headerLen, totalMsgSize);

        size_t used = headerLen + payloadLen;
        m_pending.erase(m_pending.begin(), m_pending.begin() + used);
        m_pendingOffset += used;
    }

    if (m_pending.size() > MAX_PENDING) {
        // No plausible frame boundary in a lot of data. Keep the bytes in the
        // stream file, but stop trying to parse them.
        fprintf(m_indexFile, "%llu,%s,%llu,UNPARSED,-,-,-,-,-,%zu,,\n",
            (unsigned long long) elapsedMs(), m_name, (unsigned long long) m_pendingOffset, m_pending.size());

        m_pendingOffset += m_pending.size();
        m_pending.clear();
    }
}

void FrameRecorder::logFrame(uint64_t offset, const unsigned char* header, size_t headerLen, uint32_t totalMsgSize) {
    unsigned char flags = header[1];
    unsigned char frameType = flags & FRAME_TYPE_MASK;
    size_t payloadLen = readBe16(header + 2);
    char hex[33] = {0};
    size_t hexLen = std::min(payloadLen, sizeof(hex) / 2 - 1);

    appendHex(hex, sizeof(hex), header + headerLen, hexLen);

    fprintf(m_indexFile, "%llu,%s,%llu,%u,%02x,%s,%u,%u,%zu,%u,%u,%s\n",
        (unsigned long long) elapsedMs(), m_name, (unsigned long long) offset,
        header[0], flags, frameTypeName(frameType),
        (unsigned int) ((flags & MESSAGE_TYPE_CONTROL) ? 1 : 0),
        (unsigned int) ((flags & ENCRYPTION_ENCRYPTED) ? 1 : 0),
        headerLen, (unsigned int) payloadLen, (unsigned int) totalMsgSize, hex);
}

uint64_t FrameRecorder::elapsedMs() const {
    uint64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    return nowMs - m_startMs;
}
