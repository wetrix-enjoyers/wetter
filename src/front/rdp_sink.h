// front end: what the display-list walker and the RSP hand to an RDP. Each
// backend's RDP implements this; the commands arrive in list order.
#pragma once

#include <cstdint>

namespace wetter::front {

constexpr uint32_t RdramSize = 8u * 1024u * 1024u;

// A vertex after the RSP: screen position, depth and 1/w, colour, texel coordinate.
struct ScreenVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;      // 0 at the near plane, 1 at the far plane
    float w = 0.0f;      // 1/w from the clip-space position
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
    float s = 0.0f;
    float t = 0.0f;
};

class RdpSink {
public:
    virtual ~RdpSink() = default;

    virtual void set_color_image(uint32_t fmt, uint32_t siz, uint32_t width, uint32_t address) = 0;
    virtual void set_depth_image(uint32_t address) = 0;
    virtual void set_texture_image(uint32_t fmt, uint32_t siz, uint32_t width, uint32_t address) = 0;
    virtual void set_tile(uint32_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint32_t tile,
                          uint32_t palette, uint32_t cm_t, uint32_t mask_t, uint32_t shift_t, uint32_t cm_s,
                          uint32_t mask_s, uint32_t shift_s) = 0;
    virtual void set_tile_size(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) = 0;
    virtual void load_block(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) = 0;
    virtual void load_tile(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) = 0;
    virtual void load_tlut(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) = 0;
    virtual void set_combine(uint32_t w0, uint32_t w1) = 0;
    virtual void set_other_mode_h(uint32_t shift, uint32_t length, uint32_t data) = 0;
    virtual void set_other_mode_l(uint32_t shift, uint32_t length, uint32_t data) = 0;
    virtual void set_env_color(uint32_t rgba) = 0;
    virtual void set_prim_color(uint32_t rgba) = 0;
    virtual void set_blend_color(uint32_t rgba) = 0;
    virtual void set_fog_color(uint32_t rgba) = 0;
    virtual void set_fill_color(uint32_t rgba) = 0;
    virtual void set_prim_depth(uint32_t value) = 0;
    virtual void set_scissor(uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) = 0;
    virtual void set_geometry_mode(uint32_t mode) = 0;

    // Rectangle coordinates are 10.2 fixed point; s/t S10.5, steps S5.10.
    virtual void fill_rect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) = 0;
    virtual void tex_rect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint32_t tile, int32_t uls,
                          int32_t ult, int32_t dsdx, int32_t dtdy, bool flip) = 0;
    virtual void triangle(const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c, bool textured,
                          uint32_t tile) = 0;

    // Frames composed so far, and the frame traces fire in; for the logs.
    virtual uint64_t frames() const = 0;
    virtual uint64_t traced_frame() const = 0;

    // Debug logs of a primitive the RSP found interesting (WETTER_TEXGEN_LOG, WETTER_FLAT_LOG).
    virtual void log_texgen_draw(const ScreenVertex* corners, uint32_t tile_index) {
        (void)corners;
        (void)tile_index;
    }
    virtual void log_flat_draw(const ScreenVertex* corners, uint32_t tile_index, uint32_t geometry_mode,
                               uint32_t area) {
        (void)corners;
        (void)tile_index;
        (void)geometry_mode;
        (void)area;
    }
};

}  // namespace wetter::front
