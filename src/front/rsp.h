// front end: the RSP -- matrices, vertex transform and lighting, texture
// generation, clipping, culling, projection to screen space.

#ifndef WETTER_FRONT_RSP_H
#define WETTER_FRONT_RSP_H

#include <cstdint>
#include <cstdio>

#include "rdp_sink.h"

namespace wetter::front {

struct RspVertex {
    int16_t x = 0;
    int16_t y = 0;
    int16_t z = 0;
    uint16_t flag = 0;
    int16_t s = 0;
    int16_t t = 0;
    uint8_t r = 255;
    uint8_t g = 255;
    uint8_t b = 255;
    uint8_t a = 255;

    float colour[3] = { 1.0f, 1.0f, 1.0f };
    float alpha = 1.0f;
    float gen_s = 0.0f;
    float gen_t = 0.0f;
    bool lit = false;        // the colour came from the lights
    bool generated = false;  // the texel coordinate came from texture generation

    float clip[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
};

struct ClipVertex {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
    float s = 0.0f, t = 0.0f;
};

class Rsp {
    struct Matrix {
        float m[16];
        bool valid = false;

        uint32_t source = 0;
    };

public:
    Rsp();

    // The game's memory, so vertex arrays and matrices can be read. Not owned.
    void set_rdram(const uint8_t* rdram) { rdram_ = rdram; }

    void set_segment(uint32_t segment, uint32_t address);
    uint32_t resolve(uint32_t address) const;

    void begin_task();

    void matrix(uint32_t address, uint32_t params);

    void force_matrix(uint32_t address);

    void pop_matrix(uint32_t word);
    void set_viewport(uint32_t address);

    // G_TEXTURE: the scale factors that turn a vertex's 16-bit s/t into texels,
    // and which tile the triangles then sample.
    void set_texture(uint32_t tile, uint32_t level, uint32_t on, int32_t sc, int32_t tc);

    void set_geometry_mode(uint32_t set_bits, uint32_t clear_bits);

    // G_VTX: `count` vertices at `address` go into the cache at `dst_index`.
    void load_vertices(uint32_t address, uint32_t count, uint32_t dst_index);

    void set_light(uint32_t index, uint32_t address);

    void set_look_at(uint32_t index, uint32_t address);

    // gSPSetLightsN's count, from G_MOVEWORD G_MW_NUMLIGHT. The count is of the
    // *non-ambient* lights: the ambient is the entry at the count's own index.
    void set_light_count(uint32_t count);

    // G_TRI1/G_TRI2/G_QUAD: three cache indices, drawn with the current state.
    void draw_triangle(uint32_t a, uint32_t b, uint32_t c, RdpSink& rdp);

    uint32_t geometry_mode() const { return geometry_mode_; }
    bool texture_enabled() const;
    void transform_vertex(RspVertex& v) const;
    uint32_t texture_tile() const { return texture_tile_; }

    // Widescreen: a viewport spanning the colour image's width (within a tenth
    // either side) clips x against +-extent * w instead of +-w, so the 3D view
    // reaches past the image's edges; the RDP places those pixels.
    void set_x_extent(float extent) { x_extent_ = extent < 1.0f ? 1.0f : extent; }
    void set_color_width(uint32_t width) { color_width_ = width != 0 ? width : 320u; }
    float clip_x_extent() const;

    // A cached vertex's screen depth as G_BRANCH_Z compares it: the viewport's
    // z in its own units (0..G_MAXZ), 16.16 like G_DEPTOZS's zval. Behind the
    // eye it is as far as can be.
    int64_t screen_z_fixed(uint32_t index) const;

    // Rejected commands and vertices, for the report: a transform that silently
    // drops work looks exactly like a game with nothing to draw.
    struct Stats {
        uint64_t vertices_transformed = 0;
        uint64_t vertices_rejected = 0;
        uint64_t triangles_submitted = 0;
        uint64_t triangles_culled = 0;   // winding, by G_CULL_FRONT/G_CULL_BACK
        uint64_t triangles_empty = 0;    // clipped away entirely
        uint64_t triangles_degenerate = 0;  // a zero-area triangle after the divide
        uint64_t triangles_clipped = 0;
        uint64_t matrices = 0;
        uint64_t force_matrices = 0;
        uint64_t viewports = 0;
        uint64_t stack_underflows = 0;   // a pop with nothing left to pop
        uint64_t stack_overflows = 0;    // a push that the ten-entry stack refused
        uint64_t lights = 0;             // gSPLight commands recorded
        uint64_t look_ats = 0;           // gSPLookAt commands recorded
        uint64_t lit_vertices = 0;       // vertices whose colour came from the lights
        uint64_t generated_vertices = 0;
    };
    const Stats& stats() const { return stats_; }

    void set_geo_log(bool on) { geo_log_enabled_ = on; }

    void set_log_window(bool open) { log_window_open_ = open; }
    void log_vertex(const ClipVertex& clip, float sx, float sy, float sz) const;

    void log_matrix(uint32_t address, uint32_t params, const Matrix& m) const;

private:
    static constexpr uint32_t ModelViewStackSize = 32;
    static constexpr uint32_t ProjectionStackSize = 2;
    Matrix modelview_[ModelViewStackSize];
    uint32_t modelview_depth_ = 0;   // index of the top entry
    Matrix projection_[ProjectionStackSize];
    uint32_t projection_depth_ = 0;
    // gSPForceMatrix replaces the product outright; the flag is cleared by the
    // next G_VTX, as it is on hardware.
    Matrix forced_;
    bool forced_pending_ = false;

    static constexpr uint32_t VertexCacheSize = 128;
    RspVertex vertices_[VertexCacheSize];

    struct Viewport {
        float scale[3] = { 160.0f, 120.0f, 0.5f };
        float translate[3] = { 160.0f, 120.0f, 0.5f };
    };
    Viewport viewport_;

    static constexpr uint32_t MaxLights = 8;
    struct Light {
        float col[3] = { 0.0f, 0.0f, 0.0f };
        float col_copy[3] = { 0.0f, 0.0f, 0.0f };
        // In the same three bytes: the direction of a directional light, or the
        // position of a point light. Read as a direction here.
        float dir[3] = { 0.0f, 0.0f, 0.0f };
        float kc = 0.0f;
        float kl = 0.0f;
        float kq = 0.0f;
    };
    Light lights_[MaxLights];
    uint32_t light_count_ = 0;
    float look_at_[2][3] = { { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } };

    // The two stages above, applied to one vertex as it enters the cache. Not
    // const: the report counts the vertices that took each path.
    void shade_vertex(RspVertex& v);

    uint32_t geometry_mode_ = 0;
    uint32_t texture_tile_ = 0;
    uint32_t texture_level_ = 0;
    bool texture_on_ = false;
    int32_t texture_scale_s_ = 0x8000;
    int32_t texture_scale_t_ = 0x8000;

    uint32_t segments_[16] = {};

    bool geo_log_enabled_ = false;
    bool log_window_open_ = true;
    mutable uint32_t geo_logged_ = 0;
    mutable uint32_t tex_state_logged_ = 0;

    const uint8_t* rdram_ = nullptr;
    Stats stats_{};
    float x_extent_ = 1.0f;
    uint32_t color_width_ = 320;

    void read_matrix(uint32_t address, Matrix& out) const;
    void compute_clip(uint32_t index, ClipVertex& out) const;
};

}  // namespace wetter::front

#endif  // WETTER_FRONT_RSP_H
