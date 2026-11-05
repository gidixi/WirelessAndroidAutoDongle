#include <cstdio>
#include <cstring>

#include "framesource.h"

#ifdef AA_SRCD_HAVE_X264
#include <x264.h>
#endif

#pragma region H264FileSource
// Pictures are split on VCL NAL boundaries: our own streams carry one slice per
// picture, so a picture is the leading parameter sets plus the next slice.
H264FileSource::H264FileSource(const std::string& path): m_path(path) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        fprintf(stderr, "cannot open %s\n", path.c_str());
        return;
    }

    std::vector<uint8_t> data;
    unsigned char buffer[65536];
    size_t readLen;
    while ((readLen = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        data.insert(data.end(), buffer, buffer + readLen);
    }
    fclose(file);

    struct NalRef {
        size_t payloadStart;
        size_t startCodeStart;
    };

    std::vector<NalRef> nals;
    for (size_t i = 0; i + 2 < data.size(); i++) {
        if (data[i] != 0 || data[i + 1] != 0) {
            continue;
        }

        size_t startCodeLength = 0;
        if (data[i + 2] == 1) {
            startCodeLength = 3;
        }
        else if (i + 3 < data.size() && data[i + 2] == 0 && data[i + 3] == 1) {
            startCodeLength = 4;
        }

        if (startCodeLength > 0) {
            nals.push_back({i + startCodeLength, i});
            i += startCodeLength - 1;
        }
    }

    Picture current;
    for (size_t index = 0; index < nals.size(); index++) {
        size_t end = index + 1 < nals.size() ? nals[index + 1].startCodeStart : data.size();
        uint8_t nalType = (uint8_t)(data[nals[index].payloadStart] & 0x1f);

        // Head units expect Annex-B, so keep the start codes.
        current.data.insert(current.data.end(), data.begin() + nals[index].startCodeStart, data.begin() + end);

        if (nalType == 5) {
            current.keyframe = true;
        }

        if (nalType == 1 || nalType == 5) {
            m_pictures.push_back(std::move(current));
            current = Picture();
        }
    }

    if (!current.data.empty()) {
        m_pictures.push_back(std::move(current));
    }
}

bool H264FileSource::nextPicture(std::vector<uint8_t>& annexB, bool& keyframe) {
    if (m_pictures.empty()) {
        return false;
    }

    const Picture& picture = m_pictures[m_next];
    m_next = (m_next + 1) % m_pictures.size();

    annexB = picture.data;
    keyframe = picture.keyframe;
    return true;
}
#pragma endregion H264FileSource

#pragma region TestPatternRenderer
namespace {
    void putPixel(uint8_t* argb, int pitch, int x, int y, uint32_t color) {
        uint8_t* pixel = argb + (size_t)y * pitch + (size_t)x * 4;
        pixel[0] = (uint8_t)(color & 0xff);           // B
        pixel[1] = (uint8_t)((color >> 8) & 0xff);    // G
        pixel[2] = (uint8_t)((color >> 16) & 0xff);   // R
        pixel[3] = 0xff;
    }

    void fillRect(uint8_t* argb, int width, int height, int pitch, int x, int y, int w, int h, uint32_t color) {
        for (int row = y; row < y + h && row < height; row++) {
            for (int col = x; col < x + w && col < width; col++) {
                if (col >= 0 && row >= 0) {
                    putPixel(argb, pitch, col, row, color);
                }
            }
        }
    }

    // Seven segments, so we can show a moving number without dragging a font in.
    void drawCounter(uint8_t* argb, int width, int height, int pitch, int x, int y, int digitSize, int value, uint32_t color) {
        static const int segmentMap[10][7] = {
            {1,1,1,1,1,1,0}, {0,1,1,0,0,0,0}, {1,1,0,1,1,0,1}, {1,1,1,1,0,0,1}, {0,1,1,0,0,1,1},
            {1,0,1,1,0,1,1}, {1,0,1,1,1,1,1}, {1,1,1,0,0,0,0}, {1,1,1,1,1,1,1}, {1,1,1,1,0,1,1},
        };

        int thickness = digitSize / 6;
        int bars = digitSize / 3;
        const int* segments = segmentMap[value % 10];

        if (segments[0]) fillRect(argb, width, height, pitch, x + bars, y, digitSize - 2 * bars, thickness, color);
        if (segments[1]) fillRect(argb, width, height, pitch, x + digitSize - thickness, y + thickness, thickness, digitSize / 2 - thickness, color);
        if (segments[2]) fillRect(argb, width, height, pitch, x + digitSize - thickness, y + digitSize / 2, thickness, digitSize / 2 - thickness, color);
        if (segments[3]) fillRect(argb, width, height, pitch, x + bars, y + digitSize - thickness, digitSize - 2 * bars, thickness, color);
        if (segments[4]) fillRect(argb, width, height, pitch, x, y + digitSize / 2, thickness, digitSize / 2 - thickness, color);
        if (segments[5]) fillRect(argb, width, height, pitch, x, y + thickness, thickness, digitSize / 2 - thickness, color);
        if (segments[6]) fillRect(argb, width, height, pitch, x + bars, y + digitSize / 2 - thickness / 2, digitSize - 2 * bars, thickness, color);
    }
}

void TestPatternRenderer::draw(uint8_t* argb, int width, int height, uint64_t frameIndex) {
    int pitch = width * 4;

    // Dark blue gradient background: cheap to encode, obviously not a test card.
    for (int y = 0; y < height; y++) {
        uint32_t color = 0xff000000u | (0x20u + (uint32_t)(y * 0x40 / (height ? height : 1))) << 16 | 0x40u << 8 | 0x60u;
        for (int x = 0; x < width; x++) {
            putPixel(argb, pitch, x, y, color);
        }
    }

    fillRect(argb, width, height, pitch, width / 16, height / 16, width / 3, height / 6, 0xff2a6a2a);
    fillRect(argb, width, height, pitch, width / 16, height / 4, width / 3, height / 6, 0xff6a2a2a);
    fillRect(argb, width, height, pitch, width / 16, height / 16 + 2 * height / 6, width / 3, height / 6, 0xff2a2a6a);

    // A bar that moves every frame: proof that the stream is live, not replayed.
    int barWidth = width / 40;
    int position = (int)((frameIndex * (width / 2)) % (width / 2));
    fillRect(argb, width, height, pitch, width / 2 + position - barWidth, height / 3, barWidth * 2, height / 3, 0xffe0e0e0);

    int counterSize = height / 4;
    for (int digit = 0; digit < 4; digit++) {
        drawCounter(argb, width, height, pitch, width - (digit + 1) * (counterSize + counterSize / 4), height - counterSize - height / 16,
            counterSize, (int)(frameIndex / (digit + 1 ? (uint64_t)(digit == 0 ? 1 : digit * 10) : 1)) % 10, 0xffffffffu);
    }
}
#pragma endregion TestPatternRenderer

#pragma region LiveEncodeSource
// Plain integer BT.601 conversion; at dashboard resolutions it costs far less
// than pulling a colour library in.
static void convertArgbToI420(const std::vector<uint8_t>& argb, uint8_t** plane, int* stride, int width, int height) {
    for (int y = 0; y < height; y++) {
        const uint8_t* row = argb.data() + (size_t)y * width * 4;
        uint8_t* luma = plane[0] + (size_t)y * stride[0];

        for (int x = 0; x < width; x++) {
            int blue = row[x * 4];
            int green = row[x * 4 + 1];
            int red = row[x * 4 + 2];

            luma[x] = (uint8_t)((77 * red + 150 * green + 29 * blue) >> 8);
        }
    }

    for (int y = 0; y < height / 2; y++) {
        const uint8_t* upper = argb.data() + (size_t)(2 * y) * width * 4;
        const uint8_t* lower = argb.data() + (size_t)(2 * y + 1) * width * 4;
        uint8_t* chromaU = plane[1] + (size_t)y * stride[1];
        uint8_t* chromaV = plane[2] + (size_t)y * stride[2];

        for (int x = 0; x < width / 2; x++) {
            int o = 4 * (2 * x);
            int red = ((upper[o + 2] + upper[o + 6] + lower[o + 2] + lower[o + 6]) + 2) / 4;
            int green = ((upper[o + 1] + upper[o + 5] + lower[o + 1] + lower[o + 5]) + 2) / 4;
            int blue = ((upper[o] + upper[o + 4] + lower[o] + lower[o + 4]) + 2) / 4;

            chromaU[x] = (uint8_t)((-43 * red - 85 * green + 128 * blue + 128) >> 8);
            chromaV[x] = (uint8_t)((128 * red - 107 * green - 21 * blue + 128) >> 8);
        }
    }
}

struct LiveEncodeSource::Impl {
    int width = 0;
    int height = 0;
    Renderer& renderer;
    uint64_t frameIndex = 0;

#ifdef AA_SRCD_HAVE_X264
    x264_param_t parameters;
    x264_t* encoder = nullptr;
    x264_picture_t pictureIn;
    x264_picture_t pictureOut;
    std::vector<uint8_t> surface;      // ARGB as rendered by the UI
    bool allocated = false;
#endif

    Impl(Renderer& rendererRef): renderer(rendererRef) {}
};

bool aaSrcdHasX264Support() {
#ifdef AA_SRCD_HAVE_X264
    return true;
#else
    return false;
#endif
}

LiveEncodeSource::LiveEncodeSource(int width, int height, int fps, int bitrateKbps, Renderer& renderer): m_impl(nullptr) {
#ifndef AA_SRCD_HAVE_X264
    (void)width; (void)height; (void)fps; (void)bitrateKbps; (void)renderer;
    fprintf(stderr, "this build has no x264 support, cannot render live\n");
#else
    m_impl = new Impl(renderer);
    m_impl->width = width;
    m_impl->height = height;

    x264_param_default_preset(&m_impl->parameters, "veryfast", "zerolatency");
    m_impl->parameters.i_width = width;
    m_impl->parameters.i_height = height;
    m_impl->parameters.i_fps_num = fps > 0 ? fps : 1;
    m_impl->parameters.i_fps_den = 1;
    m_impl->parameters.i_csp = X264_CSP_I420;
    m_impl->parameters.i_threads = 2;
    m_impl->parameters.i_bframe = 0;
#if X264_BUILD >= 153
    m_impl->parameters.b_repeat_headers = 1;   // SPS/PPS in front of every IDR
#else
    m_impl->parameters.b_repeat_sps = 1;
#endif
    m_impl->parameters.b_annexb = 1;
    m_impl->parameters.b_open_gop = 0;
    m_impl->parameters.i_keyint_max = fps * 2;
    m_impl->parameters.i_keyint_min = fps;
    m_impl->parameters.i_level_idc = 30;
    m_impl->parameters.rc.i_rc_method = X264_RC_ABR;
    m_impl->parameters.rc.i_bitrate = bitrateKbps;
    m_impl->parameters.rc.i_vbv_max_bitrate = bitrateKbps;
    m_impl->parameters.rc.i_vbv_buffer_size = bitrateKbps;

    x264_param_apply_profile(&m_impl->parameters, "baseline");

    m_impl->encoder = x264_encoder_open(&m_impl->parameters);
    if (!m_impl->encoder) {
        fprintf(stderr, "cannot open the x264 encoder\n");
        delete m_impl;
        m_impl = nullptr;
        return;
    }

    if (x264_picture_alloc(&m_impl->pictureIn, X264_CSP_I420, width, height) < 0) {
        fprintf(stderr, "cannot allocate the encoder input picture\n");
        x264_encoder_close(m_impl->encoder);
        delete m_impl;
        m_impl = nullptr;
        return;
    }

    x264_picture_init(&m_impl->pictureOut);
    m_impl->surface.resize((size_t)width * height * 4);
    m_impl->allocated = true;
#endif
}

LiveEncodeSource::~LiveEncodeSource() {
#ifdef AA_SRCD_HAVE_X264
    if (m_impl) {
        if (m_impl->allocated) {
            x264_picture_clean(&m_impl->pictureIn);
        }
        if (m_impl->encoder) {
            x264_encoder_close(m_impl->encoder);
        }
        delete m_impl;
    }
#endif
}

bool LiveEncodeSource::isReady() const {
    return m_impl != nullptr;
}

bool LiveEncodeSource::nextPicture(std::vector<uint8_t>& annexB, bool& keyframe) {
#ifdef AA_SRCD_HAVE_X264
    if (!m_impl) {
        return false;
    }

    m_impl->renderer.draw(m_impl->surface.data(), m_impl->width, m_impl->height, m_impl->frameIndex++);
    convertArgbToI420(m_impl->surface, m_impl->pictureIn.img.plane, m_impl->pictureIn.img.i_stride, m_impl->width, m_impl->height);
    m_impl->pictureIn.i_type = X264_TYPE_AUTO;

    x264_nal_t* nals = nullptr;
    int nalCount = 0;
    int encoded = x264_encoder_encode(m_impl->encoder, &nals, &nalCount, &m_impl->pictureIn, &m_impl->pictureOut);

    if (encoded < 0) {
        fprintf(stderr, "x264 encode failed\n");
        return false;
    }

    annexB.clear();
    for (int i = 0; i < nalCount; i++) {
        annexB.insert(annexB.end(), nals[i].p_payload, nals[i].p_payload + nals[i].i_payload);
    }

    keyframe = m_impl->pictureOut.b_keyframe;
    return encoded > 0;
#else
    (void)annexB; (void)keyframe;
    return false;
#endif
}
#pragma endregion LiveEncodeSource
