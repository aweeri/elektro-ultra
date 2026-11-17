#include "gui/sw_render.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "imgui.h"
#include "xrit/png.h"

namespace eu {

void Canvas::clear(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i] = r;
        rgba[i + 1] = g;
        rgba[i + 2] = b;
        rgba[i + 3] = a;
    }
}

namespace {

struct Rgba {
    float r, g, b, a;
};

inline Rgba unpack(ImU32 col) {
    Rgba c;
    c.r = (float)(col & 0xFF) / 255.0f;
    c.g = (float)((col >> 8) & 0xFF) / 255.0f;
    c.b = (float)((col >> 16) & 0xFF) / 255.0f;
    c.a = (float)((col >> 24) & 0xFF) / 255.0f;
    return c;
}

}  // namespace

void render_draw_data(ImDrawData* dd, Canvas& c, const uint8_t* tex, int tex_w,
                      int tex_h) {
    if (!dd || c.width <= 0 || c.height <= 0) return;
    const int fb_w = (int)(dd->DisplaySize.x * dd->FramebufferScale.x);
    const int fb_h = (int)(dd->DisplaySize.y * dd->FramebufferScale.y);
    if (fb_w != c.width || fb_h != c.height) c.resize(fb_w, fb_h);

    for (int li = 0; li < dd->CmdListsCount; ++li) {
        const ImDrawList* list = dd->CmdLists[li];
        for (int ci = 0; ci < list->CmdBuffer.Size; ++ci) {
            const ImDrawCmd& cmd = list->CmdBuffer[ci];
            if (cmd.UserCallback) {
                if (cmd.UserCallback != ImDrawCallback_ResetRenderState)
                    cmd.UserCallback(list, &cmd);
                continue;
            }

            const int cx0 = (int)std::floor(std::max(cmd.ClipRect.x, 0.0f));
            const int cy0 = (int)std::floor(std::max(cmd.ClipRect.y, 0.0f));
            const int cx1 = (int)std::ceil(std::min(cmd.ClipRect.z, (float)c.width));
            const int cy1 = (int)std::ceil(std::min(cmd.ClipRect.w, (float)c.height));
            if (cx1 <= cx0 || cy1 <= cy0) continue;

            for (unsigned e = 0; e < cmd.ElemCount; e += 3) {
                const ImDrawIdx* idx = &list->IdxBuffer[cmd.IdxOffset + e];
                const ImDrawVert& v0 = list->VtxBuffer[cmd.VtxOffset + idx[0]];
                const ImDrawVert& v1 = list->VtxBuffer[cmd.VtxOffset + idx[1]];
                const ImDrawVert& v2 = list->VtxBuffer[cmd.VtxOffset + idx[2]];
                const ImDrawVert* vv[3] = {&v0, &v1, &v2};

                float minx = std::min({v0.pos.x, v1.pos.x, v2.pos.x});
                float maxx = std::max({v0.pos.x, v1.pos.x, v2.pos.x});
                float miny = std::min({v0.pos.y, v1.pos.y, v2.pos.y});
                float maxy = std::max({v0.pos.y, v1.pos.y, v2.pos.y});
                int x0 = std::max(cx0, (int)std::floor(minx));
                int x1 = std::min(cx1, (int)std::ceil(maxx) + 1);
                int y0 = std::max(cy0, (int)std::floor(miny));
                int y1 = std::min(cy1, (int)std::ceil(maxy) + 1);
                if (x1 <= x0 || y1 <= y0) continue;

                const float ax = v1.pos.x - v0.pos.x, ay = v1.pos.y - v0.pos.y;
                const float bx = v2.pos.x - v0.pos.x, by = v2.pos.y - v0.pos.y;
                const float area = ax * by - ay * bx;
                if (std::fabs(area) < 1e-9f) continue;   // degenerate
                const float inv = 1.0f / area;

                for (int y = y0; y < y1; ++y) {
                    for (int x = x0; x < x1; ++x) {
                        const float px = (float)x + 0.5f - v0.pos.x;
                        const float py = (float)y + 0.5f - v0.pos.y;
                        const float u = (px * by - py * bx) * inv;
                        const float v = (py * ax - px * ay) * inv;
                        const float w = 1.0f - u - v;
                        if (u < -0.0001f || v < -0.0001f || w < -0.0001f) continue;

                        const float tu = u * vv[1]->uv.x + v * vv[2]->uv.x + w * vv[0]->uv.x;
                        const float tv = u * vv[1]->uv.y + v * vv[2]->uv.y + w * vv[0]->uv.y;
                        int tx = (int)(tu * (float)tex_w);
                        int ty = (int)(tv * (float)tex_h);
                        tx = std::clamp(tx, 0, std::max(0, tex_w - 1));
                        ty = std::clamp(ty, 0, std::max(0, tex_h - 1));
                        const uint8_t* t = tex ? &tex[((size_t)ty * tex_w + tx) * 4] : nullptr;

                        const Rgba c0 = unpack(vv[0]->col);
                        const Rgba c1 = unpack(vv[1]->col);
                        const Rgba c2 = unpack(vv[2]->col);
                        const float ca = u * c1.a + v * c2.a + w * c0.a;
                        const float cr = u * c1.r + v * c2.r + w * c0.r;
                        const float cg = u * c1.g + v * c2.g + w * c0.g;
                        const float cb = u * c1.b + v * c2.b + w * c0.b;

                        float sr = cr, sg = cg, sb = cb, sa = ca;
                        if (t) {
                            const float ta = (float)t[3] / 255.0f;
                            sr *= (float)t[0] / 255.0f;
                            sg *= (float)t[1] / 255.0f;
                            sb *= (float)t[2] / 255.0f;
                            sa *= ta;
                        }
                        if (sa <= 0.003f) continue;

                        uint8_t* dst = &c.rgba[((size_t)y * c.width + x) * 4];
                        const float ia = 1.0f - sa;
                        dst[0] = (uint8_t)std::min(255.0f, sr * 255.0f + dst[0] * ia);
                        dst[1] = (uint8_t)std::min(255.0f, sg * 255.0f + dst[1] * ia);
                        dst[2] = (uint8_t)std::min(255.0f, sb * 255.0f + dst[2] * ia);
                        dst[3] = 255;
                    }
                }
            }
        }
    }
}

bool canvas_to_png(const Canvas& c, const char* path) {
    std::vector<uint8_t> rgb((size_t)c.width * c.height * 3);
    for (size_t i = 0, j = 0; i < c.rgba.size(); i += 4, j += 3) {
        rgb[j] = c.rgba[i];
        rgb[j + 1] = c.rgba[i + 1];
        rgb[j + 2] = c.rgba[i + 2];
    }
    return png_write_rgb8(path, c.width, c.height, rgb);
}

}  // namespace eu
