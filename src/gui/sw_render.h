// No GL/X11: draw lists rasterised into an RGBA buffer.
#pragma once

#include <cstdint>
#include <vector>

struct ImDrawData;

namespace eu {

struct Canvas {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;   // width*height*4

    void resize(int w, int h) {
        width = w;
        height = h;
        rgba.assign((size_t)w * h * 4, 0);
    }
    void clear(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
};

// tex: RGBA32 font atlas, nearest-neighbour.
void render_draw_data(ImDrawData* dd, Canvas& c, const uint8_t* tex, int tex_w, int tex_h);

bool canvas_to_png(const Canvas& c, const char* path);

}  // namespace eu
