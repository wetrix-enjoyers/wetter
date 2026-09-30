// hard: the RDP on the GPU. Commands arrive from the front end in list order;
// primitives are batched and drawn into one render target per colour image,
// through shaders generated from the combiner and blender state.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "front/rdp_sink.h"
#include "front/tmem.h"
#include "gl.h"

namespace wetter::hard {

class HardRdp final : public front::RdpSink {
public:
    HardRdp(uint8_t* rdram, const gl::Api& api, bool gles, int scale, bool copy_back_every_task,
            float extension = 1.0f);
    ~HardRdp() override;

    // Compiles the base program and makes the buffers; false if the GL is unusable.
    bool init();
    void set_rdram(uint8_t* rdram) {
        rdram_ = rdram;
        tm_.set_rdram(rdram);
    }

    // Draws whatever is batched.
    void flush();
    // The end of a task: with copy_back_every_task, drawn targets go back to RDRAM.
    void end_task();

    // Picks what the VI shows -- the target holding its origin, from the
    // origin's line, as many lines as the VI scans out (or the CPU's picture in
    // RDRAM) -- and present() draws it into the default framebuffer at 4:3.
    void compose(uint32_t vi_origin, uint32_t vi_width, uint32_t vi_status, uint32_t vi_v_start,
                 uint32_t vi_y_scale);
    void present(int width, int height);
    // The shown picture read back, ARGB8888, at target resolution.
    bool read_shown(std::vector<uint32_t>& out, int& width, int& height);

    void set_color_image(uint32_t fmt, uint32_t siz, uint32_t width, uint32_t address) override;
    void set_depth_image(uint32_t address) override;
    void set_texture_image(uint32_t fmt, uint32_t siz, uint32_t width, uint32_t address) override;
    void set_tile(uint32_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint32_t tile, uint32_t palette,
                  uint32_t cm_t, uint32_t mask_t, uint32_t shift_t, uint32_t cm_s, uint32_t mask_s,
                  uint32_t shift_s) override;
    void set_tile_size(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) override;
    void load_block(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) override;
    void load_tile(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) override;
    void load_tlut(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) override;
    void set_combine(uint32_t w0, uint32_t w1) override;
    void set_other_mode_h(uint32_t shift, uint32_t length, uint32_t data) override;
    void set_other_mode_l(uint32_t shift, uint32_t length, uint32_t data) override;
    void set_env_color(uint32_t rgba) override;
    void set_prim_color(uint32_t rgba) override;
    void set_blend_color(uint32_t rgba) override;
    void set_fog_color(uint32_t rgba) override;
    void set_fill_color(uint32_t rgba) override;
    void set_prim_depth(uint32_t value) override;
    void set_scissor(uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) override;
    void set_geometry_mode(uint32_t mode) override;
    void fill_rect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) override;
    void tex_rect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint32_t tile, int32_t uls, int32_t ult,
                  int32_t dsdx, int32_t dtdy, bool flip) override;
    void triangle(const front::ScreenVertex& a, const front::ScreenVertex& b, const front::ScreenVertex& c,
                  bool textured, uint32_t tile) override;

    uint64_t frames() const override { return frames_; }
    uint64_t traced_frame() const override { return 0; }

    struct Stats {
        uint64_t triangles = 0;
        uint64_t fill_rects = 0;
        uint64_t tex_rects = 0;
        uint64_t draws = 0;               // GL draw calls
        uint64_t targets_created = 0;
        uint64_t textures_decoded = 0;
        uint64_t programs = 0;
        uint64_t write_backs = 0;         // targets copied back to RDRAM
        double draw_ms = 0;               // in flush(): state, upload, the draw call
        double texture_ms = 0;            // decoding and uploading new textures
    };
    const Stats& stats() const { return stats_; }

private:
    struct Target {
        uint32_t address = 0;
        uint32_t width = 0, height = 0;   // native pixels
        // Widescreen: the target is ext_width wide, the image centred in it with
        // pad columns either side (native pixels; ext_width - width is even).
        uint32_t ext_width = 0, pad = 0;
        uint32_t siz = 2;
        gl::GLuint fbo = 0, color = 0;
        gl::GLuint depth = 0;             // shared by targets of one size
        bool dirty = false;               // drawn since RDRAM last had its pixels
    };

    // A decoded tile as a GL texture, and how texel coordinates map onto it.
    struct Texture {
        gl::GLuint id = 0;
        float map[4] = { 0, 0, 0, 0 };   // u = s * map[0] + map[1], v = t * map[2] + map[3]
        uint64_t last_frame = 0;
    };

    // A generated program and its uniform locations.
    struct Program {
        gl::GLuint id = 0;
        gl::GLint size = -1, tex0 = -1, tex1 = -1, map0 = -1, map1 = -1, prim = -1, env = -1, blend = -1, fog = -1,
                  fill = -1, persp = -1, scale = -1, pad = -1;
        // What its uniforms hold now, so only changes are sent.
        bool primed = false;
        float held_size[2] = {}, held_pad = 0, held_scale = 0, held_persp = 0;
        float held_map[2][4] = {}, held_prim[4] = {}, held_env[4] = {}, held_blend[4] = {}, held_fog[4] = {},
              held_fill[4] = {};
    };

    // Everything a batch is drawn with; a primitive whose state differs flushes.
    struct DrawState {
        Program* program = nullptr;
        gl::GLuint tex[2] = { 0, 0 };
        float map[2][4] = {};
        float prim[4] = {}, env[4] = {}, blend[4] = {}, fog[4] = {}, fill[4] = {};
        float persp = 1.0f;
        int depth = 0;       // bit 0 test, bit 1 write
        int blending = 0;    // 0 none, 1 source alpha over the framebuffer
        bool operator==(const DrawState& o) const;
    };

    // One vertex: native-pixel position, depth 0..1, colour, s*q, t*q, q, and
    // for a texture rectangle its step: x0, y0, ds/dx, dt/dy (q < 0 marks one,
    // -2 flipped).
    static constexpr int VertexFloats = 14;

    Target* current_target();
    Target* find_shown(uint32_t origin);
    void import_rdram(Target& t);
    void write_back(Target& t);
    // Before a load reads the texture image: if a drawn target holds it, write it back.
    void sync_texture_source();
    void decode_rdram(uint32_t address, uint32_t width, uint32_t height, uint32_t siz,
                      std::vector<uint32_t>& rgba) const;
    bool bind_target_state();
    Program* program_for_state(bool textured, bool& blending);
    Texture* texture_for_tile(uint32_t tile);
    void begin_primitive(bool textured, uint32_t tile, int depth_key, float persp);
    void apply_draw_state(const DrawState& s);
    void push_vertex(float x, float y, float z, const float* rgba, float s, float t, float q,
                     const float* rect = nullptr);

    uint8_t* rdram_ = nullptr;
    const gl::Api& gl_;
    bool gles_ = false;
    int scale_ = 1;
    bool copy_back_every_task_ = false;
    float extension_ = 1.0f;   // display aspect over 4:3, at least 1

    front::Tmem tm_;
    std::map<uint32_t, Target> targets_;
    std::map<uint64_t, gl::GLuint> depth_buffers_;   // by (width << 32 | height), scaled
    Target* bound_ = nullptr;
    bool depth_clear_pending_ = false;
    const Target* shown_ = nullptr;
    uint32_t shown_y0_ = 0, shown_lines_ = 0;   // native rows of shown_ the VI scans out
    Target cpu_picture_;   // the VI showing memory no target drew
    bool shown_cpu_ = false;

    // RDP state.
    uint32_t cimg_address_ = 0, cimg_width_ = 320, cimg_siz_ = 2;
    uint32_t zimg_address_ = 0;
    uint32_t other_mode_h_ = 0, other_mode_l_ = 0;
    uint32_t combine_l_ = 0, combine_h_ = 0;
    uint32_t fill_color_ = 0;
    float prim_color_[4] = { 0, 0, 0, 0 }, env_color_[4] = { 0, 0, 0, 0 };
    float blend_color_[4] = { 0, 0, 0, 0 }, fog_color_[4] = { 0, 0, 0, 0 };
    uint32_t scissor_[4] = { 0, 0, 320 << 2, 240 << 2 };   // 10.2

    std::unordered_map<uint64_t, Texture> textures_;
    std::unordered_map<std::string, Program> programs_;
    // The program for a combiner/blender state, by a compact key, so the shader
    // text is only built for a state not seen before.
    std::unordered_map<uint64_t, std::pair<Program*, bool>> program_by_state_;
    uint64_t last_state_key_ = ~0ull;
    std::pair<Program*, bool> last_program_{ nullptr, false };
    // Each tile's texture, until a load, a tile or a mode change moves it.
    Texture* tile_texture_[8] = {};
    bool tile_texture_valid_[8] = {};
    void invalidate_tiles() {
        for (bool& v : tile_texture_valid_) v = false;
    }
    std::pair<Program*, bool> build_program();

    // GL state as last set, and the batch drawn with it.
    DrawState applied_;
    bool applied_valid_ = false;
    DrawState batch_state_;
    int scissor_set_[4] = { -1, -1, -1, -1 };
    std::vector<float> batch_;
    gl::GLuint white_ = 0;   // 1x1 white, for untextured draws

    gl::GLuint vao_ = 0, vbo_ = 0;
    gl::GLuint bound_tex_[2] = { 0, 0 };

    uint64_t frames_ = 0;
    Stats stats_{};
};

}  // namespace wetter::hard
