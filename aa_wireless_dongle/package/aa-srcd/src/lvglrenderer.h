#pragma once

#include <cstdint>
#include <vector>

#include <lvgl.h>

#include "framesource.h"

// Draws the LVGL dashboard into the ARGB buffer the encoder reads. Only one
// instance may exist per process: LVGL keeps global state.
class LvglRenderer: public Renderer {
public:
    LvglRenderer(int width, int height);
    ~LvglRenderer() override;

    void draw(uint8_t* argb, const RenderContext& context) override;

private:
    static void flushCallback(lv_display_t* display, const lv_area_t* area, uint8_t* pixels);
    static uint32_t tickCallback();

    void buildLayout();
    void refreshWidgets(const RenderContext& context);

    int m_width;
    int m_height;

    lv_display_t* m_display = nullptr;
    std::vector<uint8_t> m_storage;       // owns the memory behind m_pixels
    uint8_t* m_pixels = nullptr;          // aligned draw buffer LVGL renders into

    lv_obj_t* m_title = nullptr;
    lv_obj_t* m_gauge = nullptr;
    lv_obj_t* m_clock = nullptr;
    lv_obj_t* m_uptimeValue = nullptr;
    lv_obj_t* m_uptimeBar = nullptr;
    lv_obj_t* m_modeLabel = nullptr;
    lv_obj_t* m_drivingLabel = nullptr;
    lv_obj_t* m_videoLabel = nullptr;
    lv_obj_t* m_frameLabel = nullptr;
};
