#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Anything that can hand the daemon one complete Annex-B picture
// (parameter sets + slice data) per call. Two implementations exist: replaying a
// pre-encoded file, and rendering + encoding live.
class FrameSource {
public:
    virtual ~FrameSource() = default;

    virtual bool nextPicture(std::vector<uint8_t>& annexB, bool& keyframe) = 0;
    virtual const char* name() const = 0;

    // Checked before the session starts so we never drop the first picture,
    // which is the one carrying the parameter sets.
    virtual bool isReady() const = 0;
};

// Offline path: ffmpeg produced the stream, we just cycle through its pictures.
class H264FileSource: public FrameSource {
public:
    explicit H264FileSource(const std::string& path);

    bool nextPicture(std::vector<uint8_t>& annexB, bool& keyframe) override;
    const char* name() const override { return m_path.c_str(); }
    bool isReady() const override { return !m_pictures.empty(); }
    size_t pictureCount() const { return m_pictures.size(); }

private:
    struct Picture {
        std::vector<uint8_t> data;
        bool keyframe = false;
    };

    std::string m_path;
    std::vector<Picture> m_pictures;
    size_t m_next = 0;
};

// Everything the dashboard may show besides its own clock. The head unit pushes
// the sensor values over the sensor channel; the daemon owns the lifetime.
struct DashboardState {
    bool nightMode = false;
    int drivingStatus = -1;             // -1 unknown, otherwise the DrivingStatus enum value
    int videoWidth = 0;
    int videoHeight = 0;
    int videoFps = 0;
    uint64_t sessionStartMs = 0;
};

struct RenderContext {
    int width = 0;
    int height = 0;
    uint64_t frameIndex = 0;
    const DashboardState* state = nullptr;
};

// Surface the UI toolkit draws into: 32 bit ARGB, top-down rows.
// This is where LVGL / Dear ImGui / Qt plug in: they all can render into a
// caller supplied buffer without any window system.
class Renderer {
public:
    virtual ~Renderer() = default;

    // argb points at a width*4 pitch, top-down, 0xAARRGGBB in memory as B,G,R,A.
    virtual void draw(uint8_t* argb, const RenderContext& context) = 0;
};

// Stand-in renderer used until a real UI toolkit is wired up: proves the live
// pipeline moves pixels (moving bar, colour blocks, animated counter).
class TestPatternRenderer: public Renderer {
public:
    void draw(uint8_t* argb, const RenderContext& context) override;
};

// Live path: render with Renderer, encode with libx264.
class LiveEncodeSource: public FrameSource {
public:
    LiveEncodeSource(int width, int height, int fps, int bitrateKbps, Renderer& renderer, const DashboardState& state);
    ~LiveEncodeSource() override;

    bool nextPicture(std::vector<uint8_t>& annexB, bool& keyframe) override;
    const char* name() const override { return "live"; }

    // False when the binary was built without x264 support.
    bool isReady() const override;

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

// True when this build was compiled against libx264.
bool aaSrcdHasX264Support();
