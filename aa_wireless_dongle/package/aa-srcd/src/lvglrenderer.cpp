#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "lvglrenderer.h"

namespace {
    // LVGL refuses draw buffers that are not aligned to LV_DRAW_BUF_ALIGN, and
    // std::vector gives no such guarantee, so we align the pointer ourselves.
    constexpr size_t kAlignment = 64;

    std::vector<uint8_t> allocateAligned(size_t bytes, uint8_t*& alignedOut) {
        std::vector<uint8_t> storage(bytes + kAlignment);
        uintptr_t base = reinterpret_cast<uintptr_t>(storage.data());
        alignedOut = reinterpret_cast<uint8_t*>((base + kAlignment - 1) & ~(uintptr_t)(kAlignment - 1));
        return storage;
    }

    uint64_t steadyNowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    lv_obj_t* makeCard(lv_obj_t* parent, int xPct, int yPct, int wPct, int hPct, uint32_t color) {
        lv_obj_t* card = lv_obj_create(parent);
        lv_obj_set_pos(card, lv_pct(xPct), lv_pct(yPct));
        lv_obj_set_size(card, lv_pct(wPct), lv_pct(hPct));
        lv_obj_set_style_bg_color(card, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 10, 0);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        return card;
    }

    lv_obj_t* makeLabel(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color) {
        lv_obj_t* label = lv_label_create(parent);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
        return label;
    }

    const char* drivingText(int status) {
        if (status < 0) {
            return "DRIVING: waiting for sensor";
        }
        if (status == 0) {
            return "DRIVING: unrestricted";
        }
        if (status == 31) {
            return "DRIVING: fully restricted";
        }

        static char buffer[96];
        snprintf(buffer, sizeof(buffer), "DRIVING: masked 0x%02x%s%s%s%s%s", status,
            (status & 1) ? " no-video" : "",
            (status & 2) ? " no-keyboard" : "",
            (status & 4) ? " no-voice" : "",
            (status & 8) ? " no-config" : "",
            (status & 16) ? " short-messages" : "");
        return buffer;
    }
}

// LVGL keeps global state, so only one renderer may exist at a time. When the
// next session negotiates a different resolution we tear the old display down
// instead of re-running lv_init().
static LvglRenderer* g_activeRenderer = nullptr;
static bool g_lvglInitialized = false;

LvglRenderer::LvglRenderer(int width, int height): m_width(width), m_height(height) {
    if (g_activeRenderer && g_activeRenderer != this) {
        lv_display_delete(g_activeRenderer->m_display);
        g_activeRenderer->m_display = nullptr;
    }
    g_activeRenderer = this;

    if (!g_lvglInitialized) {
        lv_init();
        lv_tick_set_cb(tickCallback);
        g_lvglInitialized = true;
    }

    size_t surfaceBytes = (size_t)width * height * 4;
    m_storage = allocateAligned(surfaceBytes, m_pixels);
    memset(m_pixels, 0, surfaceBytes);

    m_display = lv_display_create(width, height);
    lv_display_set_color_format(m_display, LV_COLOR_FORMAT_ARGB8888);
    lv_display_set_buffers(m_display, m_pixels, nullptr, (uint32_t)surfaceBytes, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(m_display, flushCallback);
    lv_display_set_user_data(m_display, this);

    buildLayout();
}

LvglRenderer::~LvglRenderer() {
    if (m_display) {
        lv_display_delete(m_display);
        m_display = nullptr;
    }
    if (g_activeRenderer == this) {
        g_activeRenderer = nullptr;
    }
}

void LvglRenderer::flushCallback(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
    // Nothing to push anywhere: the draw buffer is the frame we hand over, and
    // draw() copies it out after the refresh finished.
    (void)area;
    (void)pixels;
    lv_display_flush_ready(display);
}

uint32_t LvglRenderer::tickCallback() {
    return (uint32_t)steadyNowMs();
}

void LvglRenderer::buildLayout() {
    lv_obj_t* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0b1220), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    bool large = m_height >= 600;
    const lv_font_t* titleFont = large ? &lv_font_montserrat_48 : &lv_font_montserrat_28;
    const lv_font_t* bodyFont = large ? &lv_font_montserrat_28 : &lv_font_montserrat_16;

    m_title = makeLabel(screen, "RASPBERRY PI ANDROID AUTO SOURCE", bodyFont, 0x9fb4cc);
    lv_obj_set_pos(m_title, lv_pct(3), lv_pct(3));

    m_clock = makeLabel(screen, "--:--:--", titleFont, 0xffffff);
    lv_obj_align(m_clock, LV_ALIGN_TOP_RIGHT, lv_pct(-3), lv_pct(3));

    lv_obj_t* gaugeCard = makeCard(screen, 3, 15, 44, 62, 0x14202e);
    m_gauge = lv_arc_create(gaugeCard);
    lv_obj_set_size(m_gauge, lv_pct(86), lv_pct(86));
    lv_arc_set_range(m_gauge, 0, 120);
    lv_arc_set_value(m_gauge, 0);
    lv_arc_set_bg_angles(m_gauge, 130, 410);
    lv_obj_set_style_arc_width(m_gauge, large ? 16 : 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(m_gauge, large ? 16 : 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(m_gauge, lv_color_hex(0x1f3348), LV_PART_MAIN);
    lv_obj_set_style_arc_color(m_gauge, lv_color_hex(0x2fa36b), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(m_gauge, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_width(m_gauge, 0, LV_PART_KNOB);
    lv_obj_remove_flag(m_gauge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(m_gauge);

    m_uptimeValue = makeLabel(m_gauge, "0", large ? &lv_font_montserrat_48 : &lv_font_montserrat_28, 0xffffff);
    lv_obj_center(m_uptimeValue);

    lv_obj_t* caption = makeLabel(gaugeCard, "SESSION SECONDS", bodyFont, 0x7f93ab);
    lv_obj_align(caption, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_t* statusCard = makeCard(screen, 51, 15, 46, 29, 0x14202e);
    m_modeLabel = makeLabel(statusCard, "LIGHT: unknown", bodyFont, 0xffffff);
    lv_obj_set_pos(m_modeLabel, 0, 0);
    m_drivingLabel = makeLabel(statusCard, drivingText(-1), bodyFont, 0x9fb4cc);
    lv_obj_set_pos(m_drivingLabel, 0, large ? 62 : 36);

    lv_obj_t* videoCard = makeCard(screen, 51, 48, 46, 29, 0x14202e);
    m_videoLabel = makeLabel(videoCard, "VIDEO -", bodyFont, 0xffffff);
    lv_obj_set_pos(m_videoLabel, 0, 0);
    m_frameLabel = makeLabel(videoCard, "FRAME 0", bodyFont, 0x9fb4cc);
    lv_obj_set_pos(m_frameLabel, 0, large ? 62 : 36);

    m_uptimeBar = lv_bar_create(screen);
    lv_obj_set_size(m_uptimeBar, lv_pct(94), large ? 16 : 10);
    lv_obj_align(m_uptimeBar, LV_ALIGN_BOTTOM_MID, 0, lv_pct(-4));
    lv_bar_set_range(m_uptimeBar, 0, 120);
    lv_bar_set_value(m_uptimeBar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(m_uptimeBar, lv_color_hex(0x1f3348), LV_PART_MAIN);
    lv_obj_set_style_bg_color(m_uptimeBar, lv_color_hex(0x3d7fd6), LV_PART_INDICATOR);
}

void LvglRenderer::refreshWidgets(const RenderContext& context) {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);

    char clock[16];
    snprintf(clock, sizeof(clock), "%02d:%02d:%02d", local.tm_hour, local.tm_min, local.tm_sec);
    lv_label_set_text(m_clock, clock);

    uint64_t uptimeSeconds = 0;
    if (context.state && context.state->sessionStartMs > 0) {
        uint64_t current = steadyNowMs();
        uptimeSeconds = current > context.state->sessionStartMs ? (current - context.state->sessionStartMs) / 1000 : 0;
    }

    int gaugeValue = (int)std::min<uint64_t>(uptimeSeconds, 120);
    char value[16];
    snprintf(value, sizeof(value), "%d", gaugeValue);
    lv_label_set_text(m_uptimeValue, value);
    lv_arc_set_value(m_gauge, gaugeValue);

    if (context.state) {
        lv_label_set_text(m_modeLabel, context.state->nightMode ? "LIGHT: night" : "LIGHT: day");
        lv_label_set_text(m_drivingLabel, drivingText(context.state->drivingStatus));

        char video[64];
        snprintf(video, sizeof(video), "VIDEO %dx%d @ %d fps",
            context.state->videoWidth, context.state->videoHeight, context.state->videoFps);
        lv_label_set_text(m_videoLabel, video);
    }

    char frame[32];
    snprintf(frame, sizeof(frame), "FRAME %llu", (unsigned long long)context.frameIndex);
    lv_label_set_text(m_frameLabel, frame);

    lv_bar_set_value(m_uptimeBar, gaugeValue, LV_ANIM_OFF);
}

void LvglRenderer::draw(uint8_t* argb, const RenderContext& context) {
    refreshWidgets(context);
    lv_timer_handler();

    memcpy(argb, m_pixels, (size_t)m_width * m_height * 4);
}
