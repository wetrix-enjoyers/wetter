// soft renderer: the RDP. See rdp.h.


#include "rdp.h"

#include "front/combine.h"
#include "hooks.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace wetter::soft {

using namespace front::cc;

double g_prof_ms[4] = { 0.0, 0.0, 0.0, 0.0 };
uint64_t g_prof_count[4] = { 0, 0, 0, 0 };


namespace {

// --- small helpers ---------------------------------------------------------

inline float from_5bit(uint32_t v) {
    const uint32_t five = v & 0x1Fu;
    return static_cast<float>((five << 3) | (five >> 2)) / 255.0f;
}
inline float from_8bit(uint32_t v) { return static_cast<float>(v & 0xFFu) / 255.0f; }
inline float from_4bit(uint32_t v) { return static_cast<float>(v & 0x0Fu) / 15.0f; }
inline float from_3bit(uint32_t v) { return static_cast<float>(v & 0x07u) / 7.0f; }

inline uint32_t to_8bit(float v) {
    const float scaled = v * 255.0f + 0.5f;
    return static_cast<uint32_t>(scaled < 0.0f ? 0.0f : (scaled > 255.0f ? 255.0f : scaled));
}

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// Combiner and blender helpers: see Rdp::run_combiner / Rdp::run_blender.

inline int32_t clamp_9bit(int32_t v) {
    v &= 0x1FF;
    if (v < 0x100) return v;
    return v < 0x180 ? 0xFF : 0;
}

inline int32_t to_channel(float v) {
    const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return static_cast<int32_t>(c * 255.0f + 0.5f);
}

}  // namespace

constexpr uint64_t SampleInterval = 300;

uint64_t Rdp::traced_frame() const {
    return trace_frame_ != 0 ? trace_frame_ : host_first_gameplay_frame();
}

bool Rdp::is_sample_frame() const {
    if (first_drawn_frame_ != 0 && stats_.frames == first_drawn_frame_) {
        return true;
    }
    if (!host_in_gameplay()) {
        return false;
    }

    const uint64_t played = host_gameplay_frames();
    return played == 1 || (played - 1) % SampleInterval == 0;
}

Rdp::Rdp(uint8_t* rdram)
    : rect_log_enabled_(std::getenv("WETTER_RECT_LOG") != nullptr),
      source_map_enabled_(std::getenv("WETTER_SOURCE_MAP") != nullptr),
      cimg_log_enabled_(std::getenv("WETTER_CIMG_LOG") != nullptr),
      vi_log_enabled_(std::getenv("WETTER_VI_LOG") != nullptr),
      no_plot_(std::getenv("WETTER_NO_PLOT") != nullptr),
      rdram_(rdram) {
    tm_.set_rdram(rdram);
    for (auto& row : span_const_) {
        for (int32_t& v : row) v = -1;
    }
    if (const char* trace_env = std::getenv("WETTER_TRACE_FRAME");
        trace_env != nullptr && *trace_env != '\0') {
        trace_frame_ = static_cast<uint64_t>(std::strtoull(trace_env, nullptr, 10));
    }
    frame_.assign(size_t(MaxFramebufferWidth) * MaxFramebufferHeight * 4, 0);
    frame_width_ = 320;
    frame_height_ = 240;
    clip_x0_ = 0;
    clip_y0_ = 0;
    clip_x1_ = MaxFramebufferWidth;
    clip_y1_ = MaxFramebufferHeight;
}

uint8_t Rdp::read_byte(uint32_t address) const {
    if (address >= RdramSize) return 0;
    return rdram_[address ^ 3u];
}

uint16_t Rdp::read_half(uint32_t address) const {
    return static_cast<uint16_t>((static_cast<uint16_t>(read_byte(address)) << 8) |
                                 read_byte(address + 1));
}

uint32_t Rdp::read_word(uint32_t address) const {
    return (static_cast<uint32_t>(read_byte(address)) << 24) |
           (static_cast<uint32_t>(read_byte(address + 1)) << 16) |
           (static_cast<uint32_t>(read_byte(address + 2)) << 8) |
           static_cast<uint32_t>(read_byte(address + 3));
}

void Rdp::write_half(uint32_t address, uint16_t value) {
    if (address + 1u >= RdramSize) return;
    rdram_[address ^ 3u] = static_cast<uint8_t>(value >> 8);
    rdram_[(address + 1u) ^ 3u] = static_cast<uint8_t>(value & 0xFFu);
}

void Rdp::set_color_image(uint32_t fmt, uint32_t siz, uint32_t width, uint32_t address) {
    color_image_.fmt = fmt;
    color_image_.siz = siz;
    color_image_.width = width;
    color_image_.address = address & 0x3FFFFFFu;
    color_image_.valid = true;

    if (cimg_log_enabled_ && cimg_log_count_ < 32) {
        ++cimg_log_count_;
        std::fprintf(stderr, "[render] SETCIMG %u: fmt %u siz %u width %u address 0x%06X\n",
                     cimg_log_count_, fmt, siz, width, color_image_.address);
        std::fflush(stderr);
    }

}

void Rdp::set_depth_image(uint32_t address) {
    depth_image_.address = address & 0x3FFFFFFu;
    depth_image_.valid = true;
}

void Rdp::set_texture_image(uint32_t fmt, uint32_t siz, uint32_t width, uint32_t address) {
    tm_.set_texture_image(fmt, siz, width, address);
}

void Rdp::set_tile(uint32_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint32_t tile,
                   uint32_t palette, uint32_t cm_t, uint32_t mask_t, uint32_t shift_t,
                   uint32_t cm_s, uint32_t mask_s, uint32_t shift_s) {
    invalidate_samplers();
    tm_.set_tile(fmt, siz, line, tmem, tile, palette, cm_t, mask_t, shift_t, cm_s, mask_s, shift_s);
}

void Rdp::set_tile_size(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    invalidate_samplers();
    tm_.set_tile_size(tile, uls, ult, lrs, lrt);
}

void Rdp::load_block(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) {
    ProfScope prof_scope(prof_ms_[3]);
    invalidate_samplers();
    tm_.load_block(tile, uls, ult, lrs, dxt);
}

void Rdp::load_tile(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    ProfScope prof_scope(prof_ms_[3]);
    invalidate_samplers();
    tm_.load_tile(tile, uls, ult, lrs, lrt);
}

void Rdp::load_tlut(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    ProfScope prof_scope(prof_ms_[3]);
    invalidate_samplers();
    tm_.load_tlut(tile, uls, ult, lrs, lrt);
}

void Rdp::log_tile_load(const char* which, const char* extent_name, uint32_t tile, uint32_t uls,
                        uint32_t ult, uint32_t lrs, uint32_t extent) {
    const Tile& t = tiles_[tile < 8 ? tile : 0];
    std::fprintf(stderr,
                 "[render] %s tile %u uls %u ult %u lrs %u %s %u | "
                 "tile[fmt %u siz %u line %u tmem %u] teximg 0x%06X width %u imgsiz %u\n",
                 which, tile, uls, ult, lrs, extent_name, extent, static_cast<unsigned>(t.fmt),
                 static_cast<unsigned>(t.siz), t.line, t.tmem, texture_image_.address,
                 texture_image_.width, static_cast<unsigned>(texture_image_.siz));
    std::fflush(stderr);
}

void Rdp::set_combine(uint32_t w0, uint32_t w1) {
    invalidate_combiner();
    combine_l_ = w0 & 0x00FFFFFFu;
    combine_h_ = w1;
}

void Rdp::set_other_mode_h(uint32_t shift, uint32_t length, uint32_t data) {
    invalidate_setup();
    const uint32_t mask = ((1u << length) - 1u) << shift;
    other_mode_.H = (other_mode_.H & ~mask) | (data & mask);
}

void Rdp::set_other_mode_l(uint32_t shift, uint32_t length, uint32_t data) {
    invalidate_combiner();
    invalidate_blender();
    const uint32_t mask = ((1u << length) - 1u) << shift;
    other_mode_.L = (other_mode_.L & ~mask) | (data & mask);
}

void Rdp::set_env_color(uint32_t rgba) {
    invalidate_combiner_colors();
    env_color_[0] = from_8bit(rgba >> 24);
    env_color_[1] = from_8bit(rgba >> 16);
    env_color_[2] = from_8bit(rgba >> 8);
    env_color_[3] = from_8bit(rgba);
}

void Rdp::set_prim_color(uint32_t rgba) {
    invalidate_combiner_colors();
    prim_color_[0] = from_8bit(rgba >> 24);
    prim_color_[1] = from_8bit(rgba >> 16);
    prim_color_[2] = from_8bit(rgba >> 8);
    prim_color_[3] = from_8bit(rgba);
}

void Rdp::set_blend_color(uint32_t rgba) {
    invalidate_blender();
    blend_color_[0] = from_8bit(rgba >> 24);
    blend_color_[1] = from_8bit(rgba >> 16);
    blend_color_[2] = from_8bit(rgba >> 8);
    blend_color_[3] = from_8bit(rgba);
}

void Rdp::set_fog_color(uint32_t rgba) {
    invalidate_blender();
    fog_color_[0] = from_8bit(rgba >> 24);
    fog_color_[1] = from_8bit(rgba >> 16);
    fog_color_[2] = from_8bit(rgba >> 8);
    fog_color_[3] = from_8bit(rgba);
}

void Rdp::set_fill_color(uint32_t rgba) {
    invalidate_combiner_colors();
    fill_raw_ = rgba;
    if (color_image_.siz == 3u) {
        fill_color_[0] = from_8bit(rgba >> 24);
        fill_color_[1] = from_8bit(rgba >> 16);
        fill_color_[2] = from_8bit(rgba >> 8);
    } else {
        const uint32_t c = rgba & 0xFFFFu;
        fill_color_[0] = from_5bit(c >> 11);
        fill_color_[1] = from_5bit(c >> 6);
        fill_color_[2] = from_5bit(c >> 1);
    }
    fill_color_[3] = 1.0f;
}

void Rdp::set_prim_depth(uint32_t value) {
    prim_depth_ = static_cast<float>(value & 0xFFFFu) / 65536.0f;
}

void Rdp::set_scissor(uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    clip_x0_ = fixed_to_pixel_floor(ulx);
    clip_y0_ = fixed_to_pixel_floor(uly);
    clip_x1_ = fixed_to_pixel_ceil(lrx);
    clip_y1_ = fixed_to_pixel_ceil(lry);
}

void Rdp::set_geometry_mode(uint32_t mode) { geometry_mode_ = mode; }

bool Rdp::framebuffer_contains(uint32_t address) const {
    // The colour image is not bounded by a length in any command, so the only
    // honest bound is the machine's own memory.
    return address < RdramSize;
}

uint16_t Rdp::pack_rgba16(float r, float g, float b, float a) {
    const uint32_t r5 = to_8bit(clamp01(r)) >> 3;
    const uint32_t g5 = to_8bit(clamp01(g)) >> 3;
    const uint32_t b5 = to_8bit(clamp01(b)) >> 3;
    const uint32_t a1 = clamp01(a) >= 0.5f ? 1u : 0u;
    return static_cast<uint16_t>((r5 << 11) | (g5 << 6) | (b5 << 1) | a1);
}

void Rdp::unpack_rgba16(uint16_t value, float* r, float* g, float* b, float* a) {
    *r = from_5bit(value >> 11);
    *g = from_5bit(value >> 6);
    *b = from_5bit(value >> 1);
    *a = (value & 1u) ? 1.0f : 0.0f;
}

void Rdp::plot(int32_t x, int32_t y, uint16_t color) {
    static const bool disabled = std::getenv("WETTER_NO_PLOT") != nullptr;
    if (disabled) return;

    if (x < clip_x0_ || x >= clip_x1_ || y < clip_y0_ || y >= clip_y1_) return;
    if (!color_image_.valid || color_image_.siz != 2) return;
    if (x < 0 || y < 0 || x >= MaxFramebufferWidth || y >= MaxFramebufferHeight) return;

    const uint32_t stride = color_image_.width != 0 ? color_image_.width : 320u;
    const uint32_t address = color_image_.address + (static_cast<uint32_t>(y) * stride +
                                                    static_cast<uint32_t>(x)) * 2u;
    if (!framebuffer_contains(address + 1u)) return;

    if (!extent_reported_) {
        extent_reported_ = true;
        std::fprintf(stderr,
                     "[render] first plot: cimg 0x%06X width %u siz %u, clip %d,%d..%d,%d, "
                     "address 0x%06X\n",
                     color_image_.address, color_image_.width, color_image_.siz, clip_x0_, clip_y0_,
                     clip_x1_, clip_y1_, address);
        std::fflush(stderr);
    }
    if (address < extent_low_) extent_low_ = address;
    if (address + 1u > extent_high_) extent_high_ = address + 1u;
    if (y > max_y_) max_y_ = y;
    if (x > max_x_) max_x_ = x;

    const uint32_t index = static_cast<uint32_t>(y) * stride + static_cast<uint32_t>(x);
    if (index >= stride * FramebufferHeight) {
        ++outside_writes_;
        return;
    }

    write_half(address, color);

    if (source_map_enabled_) {
        pixel_source_[static_cast<size_t>(y) * MaxFramebufferWidth + static_cast<size_t>(x)] =
            draw_serial_;
    }

    ++plots_;
    ++stats_.pixels_drawn;
}

uint32_t Rdp::begin_draw(uint32_t kind, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t v0,
                         uint32_t v1, uint32_t v2, uint32_t v3) {
    if (!source_map_enabled_) return 0;
    draws_.push_back(DrawRecord{ kind, x0, y0, x1, y1, v0, v1, v2, v3 });
    draw_serial_ = static_cast<uint32_t>(draws_.size());
    return draw_serial_;
}

// Cleared at the end of every composed frame, so a serial means one draw of the
// frame the capture is of rather than a draw from somewhere in the run.
void Rdp::reset_source_map() {
    draws_.clear();
    draw_serial_ = 0;
    const size_t pixels = static_cast<size_t>(MaxFramebufferWidth) * MaxFramebufferHeight;
    if (pixel_source_.size() != pixels) pixel_source_.assign(pixels, 0u);
    else std::fill(pixel_source_.begin(), pixel_source_.end(), 0u);
}

void Rdp::dump_source_map(std::FILE* out) {
    std::fprintf(out,
                 "# source map for %dx%d: per row, runs of \"x0-x1 dN\" (pixels with no draw\n"
                 "# this frame have d0). The legend below is *every* draw of the frame --\n"
                 "# the .txt traces are capped, this is not -- so any dN here resolves.\n",
                 frame_width_, frame_height_);
    for (size_t i = 0; i < draws_.size(); ++i) {
        const DrawRecord& r = draws_[i];
        if (r.kind == 1u) {
            std::fprintf(out, "d#%zu fill (%d,%d)-(%d,%d) rgb8 %u %u %u word 0x%08X\n", i + 1, r.x0,
                         r.y0, r.x1, r.y1, (r.v0 >> 16) & 0xFFu, (r.v0 >> 8) & 0xFFu, r.v0 & 0xFFu,
                         r.v1);
        } else if (r.kind == 2u) {
            std::fprintf(out,
                         "d#%zu rect (%d,%d)-(%d,%d) tile %u fmt %u siz %u line %u teximg 0x%06X "
                         "st (%u,%u) d (%u,%u)\n",
                         i + 1, r.x0, r.y0, r.x1, r.y1, r.v0 & 0xFFu, (r.v0 >> 24) & 0xFu,
                         (r.v0 >> 20) & 0xFu, (r.v0 >> 8) & 0xFFFu, r.v1, (r.v2 >> 16) & 0xFFFFu,
                         r.v2 & 0xFFFFu, (r.v3 >> 16) & 0xFFFFu, r.v3 & 0xFFFFu);
        } else {
            std::fprintf(out,
                         "d#%zu tri (%d,%d)-(%d,%d) textured %u tile %u fmt %u siz %u line %u "
                         "teximg 0x%06X combineH 0x%06X cycle %u prim8 %u %u %u\n",
                         i + 1, r.x0, r.y0, r.x1, r.y1, (r.v0 >> 7) & 1u, r.v0 & 0x7u,
                         (r.v0 >> 24) & 0xFu, (r.v0 >> 20) & 0xFu, (r.v0 >> 8) & 0xFFFu, r.v1,
                         r.v2, (r.v3 >> 24) & 0x3u, (r.v3 >> 16) & 0xFFu, (r.v3 >> 8) & 0xFFu,
                         r.v3 & 0xFFu);
        }
    }
    const int height = frame_height_;
    const int width = std::min(frame_width_, static_cast<int>(MaxFramebufferWidth));
    for (int y = 0; y < height; ++y) {
        const uint32_t* row = pixel_source_.data() + static_cast<size_t>(y) * MaxFramebufferWidth;
        std::fprintf(out, "y%-3d", y);
        int x = 0;
        while (x < width) {
            const uint32_t serial = row[x];
            int end = x;
            while (end + 1 < width && row[end + 1] == serial) ++end;
            std::fprintf(out, " %d-%d d%u", x, end, serial);
            x = end + 1;
        }
        std::fputc('\n', out);
    }
    std::fflush(out);
}

uint16_t Rdp::sample(int32_t x, int32_t y) const {
    if (x < 0 || y < 0 || x >= MaxFramebufferWidth || y >= MaxFramebufferHeight) return 0;
    const uint32_t stride = color_image_.width != 0 ? color_image_.width : 320u;
    const uint32_t address = color_image_.address + (static_cast<uint32_t>(y) * stride +
                                                    static_cast<uint32_t>(x)) * 2u;
    if (address + 1u >= RdramSize) return 0;
    return read_half(address);
}

namespace {

inline int32_t expand5(uint32_t v) {
    const uint32_t five = v & 0x1Fu;
    return static_cast<int32_t>((five << 3) | (five >> 2));
}
inline int32_t expand4(uint32_t v) { return static_cast<int32_t>((v & 0x0Fu) * 17u); }
inline int32_t expand3(uint32_t v) { return static_cast<int32_t>(((v & 0x07u) * 255u + 3u) / 7u); }

inline int32_t fast_floor(float v) {
    const int32_t i = static_cast<int32_t>(v);
    return v < static_cast<float>(i) ? i - 1 : i;
}

inline uint16_t pack16(const int32_t* c) {
    return static_cast<uint16_t>(((static_cast<uint32_t>(c[0]) >> 3) << 11) |
                                 ((static_cast<uint32_t>(c[1]) >> 3) << 6) |
                                 ((static_cast<uint32_t>(c[2]) >> 3) << 1) | 1u);
}

// The RDP's tiling arithmetic as a texel index: the mask's bits are repeated,
// and mirroring folds the next bit back instead of dropping it.
inline int32_t wrap_texel_index(int32_t index, uint32_t mask, bool mirror) {
    const int32_t range = 1 << (mask & 0xFu);
    if (!mirror) return index & (range - 1);
    const int32_t folded = index & (range * 2 - 1);
    return folded >= range ? range * 2 - 1 - folded : folded;
}

}  // namespace

Rdp::Texel Rdp::decode_texel(const Tile& tile, uint32_t s, uint32_t t) {
    const uint32_t v = tm_.texel(tile, s, t, front::tlut_mode(other_mode_.H));
    return Texel{ static_cast<int32_t>(v & 0xFFu), static_cast<int32_t>((v >> 8) & 0xFFu),
                  static_cast<int32_t>((v >> 16) & 0xFFu), static_cast<int32_t>(v >> 24) };
}

bool Rdp::depth_test_enabled() const {
    return depth_image_.address != 0u && other_mode_.z_compare();
}

uint16_t Rdp::read_depth(int32_t x, int32_t y) const {
    if (depth_image_.address == 0u || x < 0 || y < 0) return 0u;
    if (static_cast<uint32_t>(x) >= MaxFramebufferWidth ||
        static_cast<uint32_t>(y) >= MaxFramebufferHeight) {
        return 0u;
    }

    const uint32_t stride = color_image_.width != 0u ? color_image_.width : 320u;
    const uint32_t address = depth_image_.address +
                             (static_cast<uint32_t>(y) * stride + static_cast<uint32_t>(x)) * 2u;
    return read_half(address);
}

void Rdp::write_depth(int32_t x, int32_t y, uint16_t value) {
    if (depth_image_.address == 0u || x < 0 || y < 0) return;
    if (static_cast<uint32_t>(x) >= MaxFramebufferWidth ||
        static_cast<uint32_t>(y) >= MaxFramebufferHeight) {
        return;
    }

    const uint32_t stride = color_image_.width != 0u ? color_image_.width : 320u;
    const uint32_t address = depth_image_.address +
                             (static_cast<uint32_t>(y) * stride + static_cast<uint32_t>(x)) * 2u;
    write_half(address, value);
}

// WETTER_TEXGEN_LOG: a generated polygon, the tile it samples through and the
// texel each of its corners resolves to. See the declaration in rdp.h.
void Rdp::log_texgen_draw(const ScreenVertex* corners, uint32_t tile_index) {
    const Tile& tile = tiles_[tile_index < 8 ? tile_index : 0];
    std::fprintf(stderr,
                 "[texgen-draw] tile %u fmt %u siz %u line %u tmem %u mask s%u t%u shift "
                 "s%u t%u cm s%u t%u uls %u ult %u lrs %u lrt %u (%ux%u texels) | teximg "
                 "fmt %u siz %u width %u 0x%06X valid %u\n",
                 tile_index < 8 ? tile_index : 0, tile.fmt, tile.siz, tile.line, tile.tmem,
                 tile.mask_s, tile.mask_t, tile.shift_s, tile.shift_t, tile.cm_s, tile.cm_t,
                 tile.uls, tile.ult, tile.lrs, tile.lrt, tile_texel_width(tile),
                 tile_texel_height(tile), texture_image_.fmt, texture_image_.siz,
                 texture_image_.width, texture_image_.address,
                 texture_image_.valid ? 1u : 0u);

    for (uint32_t i = 0; i < 3u; ++i) {
        const Texel texel = sample_tile(tile_index, corners[i].s, corners[i].t);
        std::fprintf(stderr,
                     "[texgen-draw]   corner %u screen (%.1f,%.1f) s %.3f t %.3f -> texel "
                     "%.4f %.4f %.4f a%.4f from (%u,%u)\n",
                     i, static_cast<double>(corners[i].x), static_cast<double>(corners[i].y),
                     static_cast<double>(corners[i].s), static_cast<double>(corners[i].t),
                     texel.r / 255.0, texel.g / 255.0, texel.b / 255.0, texel.a / 255.0,
                     last_sample_s_, last_sample_t_);
    }
    std::fflush(stderr);
}

void Rdp::log_flat_draw(const ScreenVertex* corners, uint32_t tile_index, uint32_t geometry_mode,
                        uint32_t area) {
    const Tile& tile = tiles_[tile_index < 8 ? tile_index : 0];

    // The three corners are sampled through the same path a pixel is, which is
    // what makes the finding a statement about the picture.
    Texel first{};
    bool same = true;
    float s[3], t[3], r[3], g[3], b[3];
    for (uint32_t i = 0; i < 3u; ++i) {
        const Texel texel = sample_tile(tile_index, corners[i].s, corners[i].t);
        s[i] = corners[i].s;
        t[i] = corners[i].t;
        r[i] = texel.r / 255.0f;
        g[i] = texel.g / 255.0f;
        b[i] = texel.b / 255.0f;
        if (i == 0u) {
            first = texel;
        } else if (texel.r != first.r || texel.g != first.g || texel.b != first.b) {
            same = false;
        }
    }

    if (!same) return;

    std::fprintf(stderr,
                 "[flat] geom 0x%X tile %u fmt %u siz %u line %u mask s%u t%u shift s%u t%u cm "
                 "s%u t%u extent %ux%u teximg 0x%06X | screen (%.0f,%.0f) (%.0f,%.0f) "
                 "(%.0f,%.0f) area %u | s %.2f %.2f %.2f t %.2f %.2f %.2f | colour %.3f "
                 "%.3f %.3f a%.3f\n",
                 geometry_mode, tile_index < 8 ? tile_index : 0, tile.fmt, tile.siz, tile.line,
                 tile.mask_s, tile.mask_t, tile.shift_s, tile.shift_t, tile.cm_s, tile.cm_t,
                 tile_texel_width(tile), tile_texel_height(tile), texture_image_.address,
                 static_cast<double>(corners[0].x), static_cast<double>(corners[0].y),
                 static_cast<double>(corners[1].x), static_cast<double>(corners[1].y),
                 static_cast<double>(corners[2].x), static_cast<double>(corners[2].y), area,
                 static_cast<double>(s[0]), static_cast<double>(s[1]), static_cast<double>(s[2]),
                 static_cast<double>(t[0]), static_cast<double>(t[1]), static_cast<double>(t[2]),
                 static_cast<double>(r[0]), static_cast<double>(g[0]), static_cast<double>(b[0]),
                 first.a / 255.0);
    std::fflush(stderr);
}

// Textures up to this many texels are decoded when their sampler is prepared.
constexpr size_t EagerTexels = 4096;

void Rdp::prepare_sampler(uint32_t tile_index) {
    const Tile& tile = tiles_[tile_index];
    SamplerSetup& sp = samplers_[tile_index];
    auto shift_scale = [](uint32_t shift) {
        if (shift == 0) return 1.0f;
        return shift <= 10 ? 1.0f / static_cast<float>(1u << shift) : static_cast<float>(1u << (16 - shift));
    };
    sp.scale_s = shift_scale(tile.shift_s);
    sp.scale_t = shift_scale(tile.shift_t);
    // Texel coordinates are relative to the tile's upper-left (sl/tl, 10.2), after the shift.
    sp.offset_s = static_cast<float>(tile.uls) / 4.0f;
    sp.offset_t = static_cast<float>(tile.ult) / 4.0f;
    sp.width = static_cast<int32_t>(tile_texel_width(tile));
    sp.height = static_cast<int32_t>(tile_texel_height(tile));
    const uint32_t range_s = 1u << (tile.mask_s & 0xFu);
    const uint32_t range_t = 1u << (tile.mask_t & 0xFu);
    sp.clamp_s = (tile.cm_s & 0x2u) != 0u || tile.mask_s == 0u;
    sp.clamp_t = (tile.cm_t & 0x2u) != 0u || tile.mask_t == 0u;
    sp.wrap_s = tile.mask_s != 0u && (!sp.clamp_s || range_s >= static_cast<uint32_t>(sp.width));
    sp.wrap_t = tile.mask_t != 0u && (!sp.clamp_t || range_t >= static_cast<uint32_t>(sp.height));
    sp.mirror_s = (tile.cm_s & 0x1u) != 0u;
    sp.mirror_t = (tile.cm_t & 0x1u) != 0u;
    sp.bilinear = ((other_mode_.H >> 12) & 3u) >= 2u && other_mode_.cycle_type() != OtherMode::G_CYC_COPY;

    // After clamping or wrapping, an index lies below the wrap range or the tile size.
    sp.cache_w = sp.wrap_s ? range_s : static_cast<uint32_t>(sp.width);
    sp.cache_h = sp.wrap_t ? range_t : static_cast<uint32_t>(sp.height);
    sp.inner_w = sp.clamp_s ? std::min(static_cast<uint32_t>(sp.width), sp.cache_w) : sp.cache_w;
    sp.inner_h = sp.clamp_t ? std::min(static_cast<uint32_t>(sp.height), sp.cache_h) : sp.cache_h;
    sp.mul_s = sp.scale_s * 256.0f;
    sp.add_s = -sp.offset_s * 256.0f;
    sp.mul_t = sp.scale_t * 256.0f;
    sp.add_t = -sp.offset_t * 256.0f;
    const size_t texels = size_t(sp.cache_w) * sp.cache_h;
    sp.cached = texels != 0 && texels <= 65536u;
    if (sp.cached && sp.cache.size() < texels) {
        sp.cache.resize(texels);
        sp.cache_gen.resize(texels, 0u);
    }
    ++sp.gen;
    sp.complete = false;
    if (sp.cached && texels <= EagerTexels) {
        auto decode_into = [&](uint32_t* out) {
            for (uint32_t t = 0; t < sp.cache_h; ++t) {
                uint32_t* row = out + size_t(t) * sp.cache_w;
                for (uint32_t s = 0; s < sp.cache_w; ++s) row[s] = pack_texel(decode_texel(tile, s, t));
            }
        };
        const size_t key = size_t(sampler_epoch_) * 8u + tile_index;
        if (shared_ != nullptr && key < SharedTexels::Slots) {
            std::atomic<uint8_t>& state = shared_->state[key];
            uint8_t expected = 0;
            if (state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
                const size_t at = shared_->used.fetch_add(texels, std::memory_order_relaxed);
                if (at + texels <= SharedTexels::ArenaTexels) {
                    uint32_t* out = shared_->arena.data() + at;
                    decode_into(out);
                    shared_->data[key] = out;
                    state.store(2, std::memory_order_release);
                } else {
                    state.store(3, std::memory_order_release);
                }
            }
            uint8_t now = state.load(std::memory_order_acquire);
            while (now == 1) {
                std::this_thread::yield();
                now = state.load(std::memory_order_acquire);
            }
            if (now == 2) {
                sp.texels = shared_->data[key];
                sp.complete = true;
            }
        }
        if (!sp.complete) {
            decode_into(sp.cache.data());
            sp.texels = sp.cache.data();
            sp.complete = true;
        }
    }
    sp.valid = true;
}

uint32_t Rdp::fetch_texel(uint32_t tile_index, uint32_t us, uint32_t ut) {
    SamplerSetup& sp = samplers_[tile_index];
    const Tile& tile = tiles_[tile_index];
    // Addressed indices lie inside the cache (see prepare_sampler).
    const size_t i = size_t(ut) * sp.cache_w + us;
    if (sp.complete) return sp.texels[i];
    if (!sp.cached) return pack_texel(decode_texel(tile, us, ut));
    if (sp.cache_gen[i] == sp.gen) return sp.cache[i];
    const uint32_t v = pack_texel(decode_texel(tile, us, ut));
    sp.cache[i] = v;
    sp.cache_gen[i] = sp.gen;
    return v;
}

namespace {

inline int32_t address_s(const int32_t width, bool clamp, bool wrap, uint32_t mask, bool mirror, int32_t i) {
    if (clamp) i = i < 0 ? 0 : (i >= width ? width - 1 : i);
    if (wrap) i = wrap_texel_index(i, mask, mirror);
    return i;
}

// Bilinear on packed RGBA8: red/blue and green/alpha as 16-bit lanes.
inline uint32_t lerp_packed(uint32_t x, uint32_t y, uint32_t f) {
    const uint32_t g = 256u - f;
    const uint32_t rb = (((x & 0x00FF00FFu) * g + (y & 0x00FF00FFu) * f + 0x00800080u) >> 8) & 0x00FF00FFu;
    const uint32_t ga = ((((x >> 8) & 0x00FF00FFu) * g + ((y >> 8) & 0x00FF00FFu) * f + 0x00800080u)) & 0xFF00FF00u;
    return rb | ga;
}

}  // namespace

uint32_t Rdp::sample_fast(uint32_t tile_index, SamplerSetup& sp, float s, float t) {
    const Tile& tile = tiles_[tile_index];
    // Texel coordinates with an 8-bit fraction.
    const int32_t fs = fast_floor(s * sp.mul_s + sp.add_s);
    const int32_t ft = fast_floor(t * sp.mul_t + sp.add_t);
    const int32_t s0 = fs >> 8, t0 = ft >> 8;
    if (sp.complete) {
        // Inside the texture no tap needs clamping or wrapping.
        const uint32_t w = sp.cache_w;
        if (!sp.bilinear) {
            if (static_cast<uint32_t>(s0) < sp.inner_w && static_cast<uint32_t>(t0) < sp.inner_h) {
                return sp.texels[size_t(t0) * w + static_cast<uint32_t>(s0)];
            }
        } else if (static_cast<uint32_t>(s0) + 1u < sp.inner_w + 0u && s0 >= 0 &&
                   static_cast<uint32_t>(t0) + 1u < sp.inner_h && t0 >= 0) {
            const uint32_t* p = sp.texels + size_t(t0) * w + static_cast<uint32_t>(s0);
            const uint32_t fx = static_cast<uint32_t>(fs & 0xFF), fy = static_cast<uint32_t>(ft & 0xFF);
            return lerp_packed(lerp_packed(p[0], p[1], fx), lerp_packed(p[w], p[w + 1], fx), fy);
        }
    }
    const uint32_t us0 = static_cast<uint32_t>(address_s(sp.width, sp.clamp_s, sp.wrap_s, tile.mask_s, sp.mirror_s, s0));
    const uint32_t ut0 = static_cast<uint32_t>(address_s(sp.height, sp.clamp_t, sp.wrap_t, tile.mask_t, sp.mirror_t, t0));
    if (!sp.bilinear) return fetch_texel(tile_index, us0, ut0);
    const uint32_t us1 = static_cast<uint32_t>(address_s(sp.width, sp.clamp_s, sp.wrap_s, tile.mask_s, sp.mirror_s, s0 + 1));
    const uint32_t ut1 = static_cast<uint32_t>(address_s(sp.height, sp.clamp_t, sp.wrap_t, tile.mask_t, sp.mirror_t, t0 + 1));
    const uint32_t fx = static_cast<uint32_t>(fs & 0xFF), fy = static_cast<uint32_t>(ft & 0xFF);
    const uint32_t top = lerp_packed(fetch_texel(tile_index, us0, ut0), fetch_texel(tile_index, us1, ut0), fx);
    const uint32_t bottom = lerp_packed(fetch_texel(tile_index, us0, ut1), fetch_texel(tile_index, us1, ut1), fx);
    return lerp_packed(top, bottom, fy);
}

Rdp::Texel Rdp::sample_tile(uint32_t tile_index, float s, float t) {
    if (tile_index >= 8 || !tiles_[tile_index].used || !texture_image_.valid) return Texel{};
    SamplerSetup& sp = samplers_[tile_index];
    if (!sp.valid) prepare_sampler(tile_index);
    const Tile& tile = tiles_[tile_index];
    stats_.texels_sampled += sp.bilinear ? 4u : 1u;
    const int32_t fs = fast_floor(s * sp.mul_s + sp.add_s);
    const int32_t ft = fast_floor(t * sp.mul_t + sp.add_t);
    last_sample_s_ = static_cast<uint32_t>(address_s(sp.width, sp.clamp_s, sp.wrap_s, tile.mask_s, sp.mirror_s, fs >> 8));
    last_sample_t_ = static_cast<uint32_t>(address_s(sp.height, sp.clamp_t, sp.wrap_t, tile.mask_t, sp.mirror_t, ft >> 8));
    const uint32_t v = sample_fast(tile_index, sp, s, t);
    return Texel{ static_cast<int32_t>(v & 0xFFu), static_cast<int32_t>((v >> 8) & 0xFFu),
                  static_cast<int32_t>((v >> 16) & 0xFFu), static_cast<int32_t>(v >> 24) };
}

namespace {

// One cycle; `sel` holds the eight operand sources (rgb a b c d, alpha a b c d).
inline void combine_one_cycle(const int32_t (*src)[4], const uint8_t* sel, int32_t out[4]) {
    for (int ch = 0; ch < 3; ++ch) {
        const int32_t a = src[sel[0]][ch], b = src[sel[1]][ch], c = src[sel[2]][ch], d = src[sel[3]][ch];
        out[ch] = clamp_9bit(((a - b) * c + d * 256 + 0x80) >> 8);
    }
    const int32_t a = src[sel[4]][3], b = src[sel[5]][3], c = src[sel[6]][3], d = src[sel[7]][3];
    out[3] = clamp_9bit(((a - b) * c + d * 256 + 0x80) >> 8);
}

}  // namespace

void Rdp::prepare_combiner() {
    if (cc_.compiled) {
        // Only a constant colour changed.
        for (int ch = 0; ch < 4; ++ch) {
            cc_.base[S_PRIM][ch] = cc_.src[S_PRIM][ch] = to_channel(prim_color_[ch]);
            cc_.base[S_ENV][ch] = cc_.src[S_ENV][ch] = to_channel(env_color_[ch]);
            cc_.fill[ch] = to_channel(fill_color_[ch]);
        }
        for (int ch = 0; ch < 4; ++ch) {
            cc_.base[S_PRIM_A][ch] = cc_.src[S_PRIM_A][ch] = cc_.base[S_PRIM][3];
            cc_.base[S_ENV_A][ch] = cc_.src[S_ENV_A][ch] = cc_.base[S_ENV][3];
        }
        span_constants_stale_ = true;
        cc_.valid = true;
        return;
    }
    std::memset(cc_.base, 0, sizeof(cc_.base));
    for (int ch = 0; ch < 4; ++ch) {
        cc_.base[S_PRIM][ch] = to_channel(prim_color_[ch]);
        cc_.base[S_ENV][ch] = to_channel(env_color_[ch]);
        cc_.base[S_ONE][ch] = 0xFF;
    }
    for (int ch = 0; ch < 4; ++ch) {
        cc_.base[S_PRIM_A][ch] = cc_.base[S_PRIM][3];
        cc_.base[S_ENV_A][ch] = cc_.base[S_ENV][3];
    }
    std::memcpy(cc_.src, cc_.base, sizeof(cc_.src));
    for (int cyc = 0; cyc < 2; ++cyc) {
        const CycleFields f = cycle_fields(combine_l_, combine_h_, cyc);
        uint8_t* s = cc_.sel[cyc];
        s[0] = kRgbA[f.rgb_a]; s[1] = kRgbB[f.rgb_b]; s[2] = kRgbC[f.rgb_c]; s[3] = kRgbD[f.rgb_d];
        s[4] = kAlphaABD[f.alpha_a]; s[5] = kAlphaABD[f.alpha_b]; s[6] = kAlphaC[f.alpha_c];
        s[7] = kAlphaABD[f.alpha_d];
    }
    // The second cycle of a two-cycle combine sees the next tile as TEXEL0.
    for (int i = 0; i < 8; ++i) cc_.sel_second[i] = swap_texels(cc_.sel[1][i]);
    cc_.cycle = other_mode_.cycle_type();
    // Compile: the _A rows are channel 3 of their colour row.
    auto base_row = [](uint8_t s, bool* alpha) {
        *alpha = true;
        switch (s) {
            case S_COMBINED_A: return uint8_t(S_COMBINED);
            case S_TEXEL0_A: return uint8_t(S_TEXEL0);
            case S_TEXEL1_A: return uint8_t(S_TEXEL1);
            case S_PRIM_A: return uint8_t(S_PRIM);
            case S_SHADE_A: return uint8_t(S_SHADE);
            case S_ENV_A: return uint8_t(S_ENV);
            default: *alpha = false; return s;
        }
    };
    for (int slot = 0; slot < 2; ++slot) {
        const uint8_t* sel = slot == 0 ? cc_.sel[0]
                                       : (cc_.cycle == OtherMode::G_CYC_2CYCLE ? cc_.sel_second : cc_.sel[1]);
        for (int ch = 0; ch < 4; ++ch) {
            const uint8_t* op = ch < 3 ? sel : sel + 4;
            for (int k = 0; k < 4; ++k) {
                bool alpha = false;
                const uint8_t row = base_row(op[k], &alpha);
                cc_.off[slot][ch][k] = static_cast<uint16_t>(row * 4 + (alpha ? 3 : ch));
                cc_.optr[slot][ch][k] = span_.rows[row][alpha ? 3 : ch];
            }
            if (op[2] == S_ZERO || op[0] == op[1]) cc_.form[slot][ch] = 1;
            else if (op[1] == S_ZERO && op[3] == S_ZERO) cc_.form[slot][ch] = 2;
            else cc_.form[slot][ch] = 0;
        }
    }
    cc_.needs_texel0 = cc_.needs_texel1 = false;
    auto note = [&](const uint8_t* sel) {
        for (int i = 0; i < 8; ++i) {
            if (sel[i] == S_TEXEL0 || sel[i] == S_TEXEL0_A) cc_.needs_texel0 = true;
            if (sel[i] == S_TEXEL1 || sel[i] == S_TEXEL1_A) cc_.needs_texel1 = true;
        }
    };
    if (cc_.cycle == OtherMode::G_CYC_2CYCLE) {
        note(cc_.sel[0]);
        note(cc_.sel_second);
    } else if (cc_.cycle == OtherMode::G_CYC_1CYCLE) {
        note(cc_.sel[1]);
    } else {
        cc_.needs_texel0 = true;   // copy mode outputs the texel
    }
    cc_.needs_shade = false;
    for (int slot = 0; slot < 2; ++slot) {
        for (int ch = 0; ch < 4; ++ch) {
            for (int k = 0; k < 4; ++k) {
                if (cc_.off[slot][ch][k] / 4 == S_SHADE) cc_.needs_shade = true;
            }
        }
    }
    for (int ch = 0; ch < 4; ++ch) cc_.fill[ch] = to_channel(fill_color_[ch]);
    span_constants_stale_ = true;
    cc_.compiled = true;
    cc_.valid = true;
}

void Rdp::combine(uint32_t t0, uint32_t t1, const int32_t* shade, int32_t* out) {
    if (!cc_.valid) prepare_combiner();
    if (cc_.cycle == OtherMode::G_CYC_COPY) {
        // Copy mode has no combiner: the texel is the output.
        out[0] = t0 & 0xFF; out[1] = (t0 >> 8) & 0xFF; out[2] = (t0 >> 16) & 0xFF; out[3] = t0 >> 24;
        return;
    }
    if (cc_.cycle == OtherMode::G_CYC_FILL) {
        for (int ch = 0; ch < 4; ++ch) out[ch] = cc_.fill[ch];
        return;
    }

    int32_t* src = &cc_.src[0][0];
    int32_t* r0 = src + S_TEXEL0 * 4;
    r0[0] = t0 & 0xFF; r0[1] = (t0 >> 8) & 0xFF; r0[2] = (t0 >> 16) & 0xFF; r0[3] = t0 >> 24;
    if (cc_.needs_texel1) {
        int32_t* r1 = src + S_TEXEL1 * 4;
        r1[0] = t1 & 0xFF; r1[1] = (t1 >> 8) & 0xFF; r1[2] = (t1 >> 16) & 0xFF; r1[3] = t1 >> 24;
    }
    if (cc_.needs_shade) {
        int32_t* rs = src + S_SHADE * 4;
        rs[0] = shade[0]; rs[1] = shade[1]; rs[2] = shade[2]; rs[3] = shade[3];
    }

    auto eval = [&](int slot, int ch) -> int32_t {
        const uint16_t* o = cc_.off[slot][ch];
        switch (cc_.form[slot][ch]) {
            case 1: return src[o[3]];
            case 2: return (src[o[0]] * src[o[2]] + 0x80) >> 8;
            default: return clamp_9bit(((src[o[0]] - src[o[1]]) * src[o[2]] + src[o[3]] * 256 + 0x80) >> 8);
        }
    };
    if (cc_.cycle == OtherMode::G_CYC_2CYCLE) {
        int32_t* rc = src + S_COMBINED * 4;
        const int32_t c0 = eval(0, 0), c1 = eval(0, 1), c2 = eval(0, 2), c3 = eval(0, 3);
        rc[0] = c0; rc[1] = c1; rc[2] = c2; rc[3] = c3;
    }
    out[0] = eval(1, 0);
    out[1] = eval(1, 1);
    out[2] = eval(1, 2);
    out[3] = eval(1, 3);
}

void Rdp::fill_span_row(int row, int32_t value) {
    if (value == 255) {
        if (span_white_[row]) return;
        span_white_[row] = true;
    }
    for (int ch = 0; ch < 4; ++ch) {
        int32_t* r = span_.rows[row][ch];
        for (int i = 0; i < SpanChunk; ++i) r[i] = value;
    }
}

void Rdp::fill_span_constants() {
    span_constants_stale_ = false;
    for (int row : { int(S_PRIM), int(S_ENV), int(S_ONE), int(S_ZERO), int(S_LOD_FRAC), int(S_PRIM_LOD_FRAC) }) {
        for (int ch = 0; ch < 4; ++ch) {
            const int32_t v = cc_.base[row][ch];
            if (span_const_[row][ch] == v) continue;
            span_const_[row][ch] = v;
            int32_t* r = span_.rows[row][ch];
            for (int i = 0; i < SpanChunk; ++i) r[i] = v;
        }
    }
}

void Rdp::combine_span(int n) {
    if (cc_.cycle == OtherMode::G_CYC_COPY) {
        for (int ch = 0; ch < 4; ++ch) std::memcpy(span_.out[ch], span_.rows[S_TEXEL0][ch], sizeof(int32_t) * n);
        return;
    }
    if (cc_.cycle == OtherMode::G_CYC_FILL) {
        for (int ch = 0; ch < 4; ++ch) {
            for (int i = 0; i < n; ++i) span_.out[ch][i] = cc_.fill[ch];
        }
        return;
    }
    auto eval = [&](int slot, int ch, int32_t* dst) {
        const int32_t* A = cc_.optr[slot][ch][0];
        const int32_t* B = cc_.optr[slot][ch][1];
        const int32_t* C = cc_.optr[slot][ch][2];
        const int32_t* D = cc_.optr[slot][ch][3];
        switch (cc_.form[slot][ch]) {
            case 1:
                for (int i = 0; i < n; ++i) dst[i] = D[i];
                break;
            case 2:
                for (int i = 0; i < n; ++i) dst[i] = (A[i] * C[i] + 0x80) >> 8;
                break;
            default:
                for (int i = 0; i < n; ++i) {
                    int32_t v = (((A[i] - B[i]) * C[i] + D[i] * 256 + 0x80) >> 8) & 0x1FF;
                    dst[i] = v < 0x100 ? v : (v < 0x180 ? 0xFF : 0);
                }
                break;
        }
    };
    if (cc_.cycle == OtherMode::G_CYC_2CYCLE) {
        int32_t tmp[4][SpanChunk];
        for (int ch = 0; ch < 4; ++ch) eval(0, ch, tmp[ch]);
        for (int ch = 0; ch < 4; ++ch) std::memcpy(span_.rows[S_COMBINED][ch], tmp[ch], sizeof(int32_t) * n);
    }
    for (int ch = 0; ch < 4; ++ch) eval(1, ch, span_.out[ch]);
}

void Rdp::prepare_blender() {
    const uint32_t bits = other_mode_.L >> 16;
    for (int cyc = 0; cyc < 2; ++cyc) {
        const uint32_t sh = cyc == 0 ? 0u : 2u;   // cycle 1 sits two bits below cycle 0
        bl_.sel[cyc][0] = (bits >> (14 - sh)) & 3u;
        bl_.sel[cyc][1] = (bits >> (10 - sh)) & 3u;
        bl_.sel[cyc][2] = (bits >> (6 - sh)) & 3u;
        bl_.sel[cyc][3] = (bits >> (2 - sh)) & 3u;
    }
    const uint32_t cycle_type = other_mode_.cycle_type();
    bl_.passthrough = cycle_type == OtherMode::G_CYC_COPY || cycle_type == OtherMode::G_CYC_FILL;
    bl_.cycles = cycle_type == OtherMode::G_CYC_2CYCLE ? 2 : 1;
    bl_.force = other_mode_.force_blend();
    bl_.reads_framebuffer = false;
    for (int cyc = 0; cyc < bl_.cycles; ++cyc) {
        const uint8_t* s = bl_.sel[cyc];
        const bool blends = cyc < bl_.cycles - 1 || bl_.force;
        if (blends && (s[0] == 1 || s[2] == 1 || s[3] == 1)) bl_.reads_framebuffer = true;
    }
    // One cycle, no forced blend, P the combined colour: the pixel goes out as is.
    // A cycle whose P and M are both the combined colour leaves it as it is,
    // whatever the weights, as does a final unforced cycle with P combined.
    bool all_identity = true;
    for (int cyc = 0; cyc < bl_.cycles; ++cyc) {
        const uint8_t* s = bl_.sel[cyc];
        const bool blends = cyc < bl_.cycles - 1 || bl_.force;
        if (!(s[0] == 0 && (!blends || s[2] == 0))) all_identity = false;
    }
    if (all_identity) bl_.passthrough = true;
    bl_.uses_shade_alpha = false;
    for (int cyc = 0; cyc < bl_.cycles; ++cyc) {
        if (bl_.sel[cyc][1] == 2) bl_.uses_shade_alpha = true;
    }
    for (int ch = 0; ch < 4; ++ch) {
        bl_.blend[ch] = to_channel(blend_color_[ch]);
        bl_.fog[ch] = to_channel(fog_color_[ch]);
    }
    bl_.valid = true;
    static const bool blend_log = std::getenv("WETTER_BLEND_LOG") != nullptr;
    if (blend_log) {
        static std::vector<uint32_t> seen;
        const uint32_t key = (other_mode_.L & 0xFFFF0000u) | (bl_.force ? 1u : 0u) |
                             (static_cast<uint32_t>(bl_.cycles) << 1) | (bl_.passthrough ? 8u : 0u);
        if (std::find(seen.begin(), seen.end(), key) == seen.end() && seen.size() < 64) {
            seen.push_back(key);
            std::fprintf(stderr,
                         "[blend] cycles %d force %d pass %d reads_fb %d | c0 p%u a%u m%u b%u | c1 p%u a%u m%u b%u\n",
                         bl_.cycles, bl_.force ? 1 : 0, bl_.passthrough ? 1 : 0, bl_.reads_framebuffer ? 1 : 0,
                         bl_.sel[0][0], bl_.sel[0][1], bl_.sel[0][2], bl_.sel[0][3], bl_.sel[1][0],
                         bl_.sel[1][1], bl_.sel[1][2], bl_.sel[1][3]);
            std::fflush(stderr);
        }
    }
}

bool Rdp::blend(int32_t* rgba, uint16_t framebuffer, int32_t shade_alpha) {
    if (!bl_.valid) prepare_blender();
    if (bl_.passthrough) return true;

    int32_t mem[4] = { 0, 0, 0, 255 };
    if (bl_.reads_framebuffer) {
        mem[0] = expand5(framebuffer >> 11);
        mem[1] = expand5(framebuffer >> 6);
        mem[2] = expand5(framebuffer >> 1);
        mem[3] = (framebuffer & 1u) ? 255 : 0;
    }
    const int32_t combiner_alpha = rgba[3];
    int32_t cur[3] = { rgba[0], rgba[1], rgba[2] };

    for (int cyc = 0; cyc < bl_.cycles; ++cyc) {
        const uint8_t p = bl_.sel[cyc][0], a = bl_.sel[cyc][1], m = bl_.sel[cyc][2], b = bl_.sel[cyc][3];
        const int32_t* pc = p == 0 ? cur : p == 1 ? mem : p == 2 ? bl_.blend : bl_.fog;
        const int32_t* mc = m == 0 ? cur : m == 1 ? mem : m == 2 ? bl_.blend : bl_.fog;
        if (cyc == bl_.cycles - 1 && !bl_.force) {
            if (p == 1) return false;   // covered pixel, P is memory: nothing changes
            cur[0] = pc[0]; cur[1] = pc[1]; cur[2] = pc[2];
            continue;
        }
        const int32_t wa = a == 0 ? combiner_alpha : a == 1 ? bl_.fog[3] : a == 2 ? shade_alpha : 0;
        const int32_t wb = b == 0 ? 255 - wa : b == 1 ? mem[3] : b == 2 ? 255 : 0;
        const int32_t sum = wa + wb;
        int32_t next[3];
        if (sum == 255) {
            for (int ch = 0; ch < 3; ++ch) {
                const int32_t v = pc[ch] * wa + mc[ch] * wb + 128;
                next[ch] = (v + (v >> 8)) >> 8;
            }
        } else if (sum > 0) {
            const int64_t recip = ((int64_t(1) << 24) + (sum >> 1)) / sum;
            for (int ch = 0; ch < 3; ++ch) {
                next[ch] = static_cast<int32_t>(((pc[ch] * wa + mc[ch] * wb) * recip + (int64_t(1) << 23)) >> 24);
            }
        } else {
            for (int ch = 0; ch < 3; ++ch) next[ch] = mc[ch];
        }
        cur[0] = next[0]; cur[1] = next[1]; cur[2] = next[2];
    }
    rgba[0] = cur[0]; rgba[1] = cur[1]; rgba[2] = cur[2]; rgba[3] = 255;
    return true;
}

uint16_t Rdp::rd16(uint32_t address) const {
    uint16_t v;
    std::memcpy(&v, rdram_ + (address ^ 2u), 2);
    return v;
}

void Rdp::wr16(uint32_t address, uint16_t value) { std::memcpy(rdram_ + (address ^ 2u), &value, 2); }

bool Rdp::row_fast(int32_t y, int32_t x0, int32_t x1, bool depth) const {
    if (no_plot_ || source_map_enabled_ || !extent_reported_) return false;
    if (!color_image_.valid || color_image_.siz != 2) return false;
    const uint32_t stride = color_image_.width != 0 ? color_image_.width : 320u;
    if (y < 0 || y < clip_y0_ || y >= clip_y1_ || y >= MaxFramebufferHeight ||
        static_cast<uint32_t>(y) >= FramebufferHeight) {
        return false;
    }
    if (x0 < 0 || x0 < clip_x0_ || x1 >= clip_x1_ || x1 >= MaxFramebufferWidth ||
        static_cast<uint32_t>(x1) >= stride) {
        return false;
    }
    const uint32_t end = (static_cast<uint32_t>(y) * stride + static_cast<uint32_t>(x1)) * 2u + 2u;
    if (color_image_.address + end > RdramSize) return false;
    if (depth && depth_image_.address + end > RdramSize) return false;
    return true;
}

void Rdp::note_span(int32_t y, int32_t x0, int32_t x1, uint32_t written) {
    if (written == 0) return;
    const uint32_t stride = color_image_.width != 0 ? color_image_.width : 320u;
    const uint32_t row = color_image_.address + static_cast<uint32_t>(y) * stride * 2u;
    const uint32_t lo = row + static_cast<uint32_t>(x0) * 2u, hi = row + static_cast<uint32_t>(x1) * 2u + 1u;
    if (lo < extent_low_) extent_low_ = lo;
    if (hi > extent_high_) extent_high_ = hi;
    if (y > max_y_) max_y_ = y;
    if (x1 > max_x_) max_x_ = x1;
    plots_ += written;
    stats_.pixels_drawn += written;
}

void Rdp::fill_rect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    ProfScope prof_scope(prof_ms_[0]);
    ++stats_.fill_rects;

    static const bool fills_disabled = std::getenv("WETTER_NO_FILL") != nullptr;
    if (fills_disabled) return;

    static const bool fill_log_all = std::getenv("WETTER_FILL_LOG") != nullptr;
    if (rect_log_enabled_ && stats_.fill_rects <= RectLogCount) {
        std::fprintf(stderr,
                     "[render] fillrect %d: (%d,%d)-(%d,%d) cimg 0x%06X colour %.2f %.2f "
                     "%.2f\n",
                     stats_.fill_rects, ulx, uly, lrx, lry, color_image_.address, fill_color_[0],
                     fill_color_[1], fill_color_[2]);
        std::fflush(stderr);
    }
    if (fill_log_all) {
        const uint64_t frame = stats_.frames;
        const bool interesting = frame == traced_frame();
        static unsigned fill_logged = 0;
        if (interesting && fill_logged < 400) {
            static uint64_t last_frame = 0;
            if (frame != last_frame) {
                last_frame = frame;
                fill_logged = 0;
                std::fprintf(stderr, "[render] --- fills of frame %llu begin, cimg 0x%06X ---\n",
                             (unsigned long long)frame, color_image_.address);
            }
            ++fill_logged;
            std::fprintf(stderr,
                         "[render] fill %u f%llu: (%d,%d)-(%d,%d) px (%d,%d)-(%d,%d) colour "
                         "%.2f %.2f %.2f\n",
                         fill_logged, (unsigned long long)frame, ulx, uly, lrx, lry,
                         fixed_to_pixel_floor(ulx), fixed_to_pixel_floor(uly),
                         fixed_to_pixel_ceil(lrx), fixed_to_pixel_ceil(lry), fill_color_[0],
                         fill_color_[1], fill_color_[2]);
            std::fflush(stderr);
        }
    }

    uint32_t right = lrx;
    uint32_t bottom = lry;
    const uint32_t cycle = other_mode_.cycle_type();
    if (cycle == OtherMode::G_CYC_FILL || cycle == OtherMode::G_CYC_COPY) {
        right |= 3u;
        bottom |= 3u;
    }

    const int32_t x0 = fixed_to_pixel_floor(ulx);
    const int32_t y0 = fixed_to_pixel_floor(uly);
    const int32_t x1 = fixed_to_pixel_ceil(right);
    const int32_t y1 = fixed_to_pixel_ceil(bottom);
    if (x1 <= x0 || y1 <= y0) return;

    const uint32_t packed_rgb =
        (to_8bit(fill_color_[0]) << 16) | (to_8bit(fill_color_[1]) << 8) | to_8bit(fill_color_[2]);
    begin_draw(1u, x0, y0, x1 - 1, y1 - 1, packed_rgb, fill_raw_, 0u, 0u);

    const uint16_t color = pack_rgba16(fill_color_[0], fill_color_[1], fill_color_[2], 1.0f);
    for (int32_t y = first_row(y0); y < y1; y = next_row(y)) {
        for (int32_t x = x0; x < x1; ++x) {
            plot(x, y, color);
        }
    }
}

void Rdp::arm_capture(uint64_t frame, const std::string& ppm_path) {
    // `frame` is the draw frame: the traces fire while the counter reads it, and
    // the picture written is the one those draws compose, which is one later.
    trace_frame_ = frame;
    capture_frame_ = frame + 1u;
    capture_armed_ = true;
    source_map_enabled_ = true;
    reset_source_map();
    traced_ = 0;
    tri_traced_ = 0;

    capture_stem_ = ppm_path;
    if (trace_out_ != stderr) std::fclose(trace_out_);
    trace_out_ = std::tmpfile();
    if (trace_out_ == nullptr) trace_out_ = stderr;

    std::fprintf(stderr,
                 "[render] capture armed: draws while the counter reads %llu, picture "
                 "composed as %llu; files named from the picture's fingerprint\n",
                 static_cast<unsigned long long>(trace_frame_),
                 static_cast<unsigned long long>(capture_frame_));
    std::fflush(stderr);
}

void Rdp::tex_rect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint32_t tile,
                   int32_t uls, int32_t ult, int32_t dsdx, int32_t dtdy, bool flip) {
    ProfScope prof_scope(prof_ms_[1]);
    ++stats_.tex_rects;

    if (rect_log_enabled_ && tex_rect_log_count_ < TexRectLogCount) {
        ++tex_rect_log_count_;
        const Tile& logged = tiles_[tile < 8 ? tile : 0];
        std::fprintf(stderr,
                     "[render] texrect %u: (%d,%d)-(%d,%d) tile %u st (%d,%d) d (%d,%d) "
                     "tile[line %u uls %u ult %u lrs %u lrt %u shift %u,%u mask %u,%u fmt %u siz %u] "
                     "teximg 0x%06X width %u cimg 0x%06X\n",
                     stats_.tex_rects, ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy,
                     logged.line, logged.uls, logged.ult, logged.lrs, logged.lrt, logged.shift_s,
                     logged.shift_t, logged.mask_s, logged.mask_t, logged.fmt, logged.siz,
                     texture_image_.address, texture_image_.width, color_image_.address);
        std::fflush(stderr);
    }
    if (lrx <= ulx || lry <= uly) return;

    const int32_t x0 = fixed_to_pixel_floor(static_cast<uint32_t>(ulx));
    const int32_t y0 = fixed_to_pixel_floor(static_cast<uint32_t>(uly));
    const int32_t x1 = fixed_to_pixel_ceil(static_cast<uint32_t>(lrx));
    const int32_t y1 = fixed_to_pixel_ceil(static_cast<uint32_t>(lry));
    if (x1 <= x0 || y1 <= y0) return;

    {
        const Tile& d = tiles_[tile < 8 ? tile : 0];
        begin_draw(2u, x0, y0, x1 - 1, y1 - 1,
                   (d.fmt << 24) | (d.siz << 20) | ((d.line & 0xFFFu) << 8) | (tile & 0xFFu),
                   texture_image_.address,
                   (static_cast<uint32_t>(uls) << 16) | (static_cast<uint32_t>(ult) & 0xFFFFu),
                   (static_cast<uint32_t>(dsdx) << 16) | (static_cast<uint32_t>(dtdy) & 0xFFFFu));
    }

    const float origin_s = static_cast<float>(uls) / 32.0f;
    const float origin_t = static_cast<float>(ult) / 32.0f;
    const float step_s = static_cast<float>(dsdx) / 1024.0f;
    const float step_t = static_cast<float>(dtdy) / 1024.0f;

    static const bool trace = std::getenv("WETTER_TEXEL_TRACE") != nullptr;
    const bool tracing =
        (trace || capture_armed_) && stats_.frames == traced_frame() && traced_ < 200;
    if (tracing) ++traced_;
    uint32_t written = 0;
    int64_t written_sum[3] = { 0, 0, 0 };
    Texel mid_texel;
    int32_t mid_combiner_alpha = 0;
    int32_t mid_final_alpha = 0;
    uint32_t mid_s = 0;
    uint32_t mid_t = 0;

    const int32_t mid_x = (x0 + x1) / 2;
    const int32_t mid_y = (y0 + y1) / 2;

    const bool read_fb = blender_reads_framebuffer();
    const bool alpha_threshold = other_mode_.alpha_compare() == OtherMode::G_AC_THRESHOLD;
    const int32_t threshold = bl_.blend[3];
    const uint32_t stride = color_image_.width != 0 ? color_image_.width : 320u;
    const int32_t shade[4] = { 255, 255, 255, 255 };
    for (int32_t y = first_row(y0); y < y1; y = next_row(y)) {
        const bool fast = row_fast(y, x0, x1 - 1, false);
        const uint32_t crow = color_image_.address + static_cast<uint32_t>(y) * stride * 2u;
        const float py = static_cast<float>(y - y0);
        uint32_t row_written = 0;
        for (int32_t x = x0; x < x1; ++x) {
            const float px = static_cast<float>(x - x0);
            const float s = origin_s + (flip ? py : px) * step_s;
            const float t = origin_t + (flip ? px : py) * step_t;

            const Texel texel = sample_tile(tile, s, t);
            int32_t rgba[4];
            combine(pack_texel(texel), 0xFFFFFFFFu, shade, rgba);
            const int32_t combiner_alpha = rgba[3];

            const uint16_t mem = read_fb ? (fast ? rd16(crow + static_cast<uint32_t>(x) * 2u) : sample(x, y)) : 1u;
            const bool keep = blend(rgba, mem, 255);

            if (tracing && x == mid_x && y == mid_y) {
                mid_texel = texel;
                mid_combiner_alpha = combiner_alpha;
                mid_final_alpha = keep ? rgba[3] : -1;
                mid_s = last_sample_s();
                mid_t = last_sample_t();
            }

            if (!keep) continue;
            if (alpha_threshold && combiner_alpha < threshold) continue;

            if (tracing) {
                ++written;
                written_sum[0] += rgba[0];
                written_sum[1] += rgba[1];
                written_sum[2] += rgba[2];
            }

            const uint16_t color = pack16(rgba);
            if (fast) {
                wr16(crow + static_cast<uint32_t>(x) * 2u, color);
                ++row_written;
            } else {
                plot(x, y, color);
            }
        }
        if (fast) note_span(y, x0, x1 - 1, row_written);
    }

    if (tracing) {
        const Tile& traced_tile = tiles_[tile < 8 ? tile : 0];
        const double count = (written != 0 ? static_cast<double>(written) : 1.0) * 255.0;
        const int32_t area = (x1 - x0) * (y1 - y0);
        std::fprintf(trace_out_,
                     "[render] rect %u: (%d,%d)-(%d,%d) %dx%d tile %u fmt %u siz %u line %u "
                     "clamp %ux%u st (%d,%d) d (%d,%d) teximg 0x%06X | wrote %u/%d avg %.2f "
                     "%.2f %.2f | mid texel %.2f %.2f %.2f a%.2f at (%u,%u) -> combiner "
                     "a%.2f, blender a%.2f\n",
                     traced_, x0, y0, x1, y1, x1 - x0, y1 - y0, tile,
                     static_cast<unsigned>(traced_tile.fmt), static_cast<unsigned>(traced_tile.siz),
                     static_cast<unsigned>(traced_tile.line), tile_texel_width(traced_tile),
                     tile_texel_height(traced_tile), uls, ult, dsdx, dtdy,
                     texture_image_.address, written, area, written_sum[0] / count,
                     written_sum[1] / count, written_sum[2] / count, mid_texel.r / 255.0,
                     mid_texel.g / 255.0, mid_texel.b / 255.0, mid_texel.a / 255.0, mid_s, mid_t,
                     mid_combiner_alpha / 255.0, mid_final_alpha / 255.0);
        std::fflush(trace_out_);
    }
}

// Per-triangle state for the span loop. Attributes are planes over the screen:
// 0 1/w, 1 s/w, 2 t/w (perspective), 3 z, 4..7 shade rgba (linear, as the RDP
// interpolates them).
struct Rdp::TriSetup {
    const ScreenVertex* a = nullptr;
    int32_t min_x = 0, max_x = 0, min_y = 0, max_y = 0;
    float inv_area = 0.0f, dw0 = 0.0f, dw1 = 0.0f, dw2 = 0.0f;
    float base[8] = {}, dx[8] = {}, dy[8] = {};
    const ScreenVertex* b = nullptr;
    const ScreenVertex* c = nullptr;
    uint32_t tile = 0;
    float persp_scale = 1.0f;
    bool alpha_threshold = false, cvg_x_alpha = false, read_fb = false, depth_write = false;
    bool tracing = false;
    int32_t mid_x = 0, mid_y = 0;
    uint32_t written = 0;
    int64_t sum[3] = { 0, 0, 0 };
    Texel mid_texel;
};

template <bool Textured, bool TwoTexels, bool Depth>
void Rdp::raster_triangle(TriSetup& ts) {
    const ScreenVertex& a = *ts.a;
    const ScreenVertex& b = *ts.b;
    const ScreenVertex& c = *ts.c;
    const uint32_t stride = color_image_.width != 0 ? color_image_.width : 320u;
    const int32_t threshold = bl_.blend[3];
    const bool need_shade = cc_.needs_shade || bl_.uses_shade_alpha;
    const bool passthrough = bl_.passthrough;
    SpanWork& sw = span_;

    // Samplers resolved once; a tile with nothing loaded samples white.
    const uint32_t tile1 = (ts.tile + 1u) & 7u;
    SamplerSetup* sp0 = nullptr;
    SamplerSetup* sp1 = nullptr;
    if constexpr (Textured) {
        if (ts.tile < 8 && tiles_[ts.tile].used && texture_image_.valid) {
            sp0 = &samplers_[ts.tile];
            if (!sp0->valid) prepare_sampler(ts.tile);
        }
        if (TwoTexels && tiles_[tile1].used && texture_image_.valid) {
            sp1 = &samplers_[tile1];
            if (!sp1->valid) prepare_sampler(tile1);
        }
    }
    if (span_constants_stale_) fill_span_constants();
    // Rows no stage below writes still hold the last triangle's pixels.
    if (!Textured || sp0 == nullptr) fill_span_row(S_TEXEL0, 255);
    else span_white_[S_TEXEL0] = false;
    if (!TwoTexels || sp1 == nullptr) fill_span_row(S_TEXEL1, 255);
    else span_white_[S_TEXEL1] = false;
    if (!need_shade) fill_span_row(S_SHADE, 255);
    else span_white_[S_SHADE] = false;

    uint64_t walked = 0, covered = 0, written = 0;

    for (int32_t y = first_row(ts.min_y); y <= ts.max_y; y = next_row(y)) {
        const float py = static_cast<float>(y) + 0.5f;
        const float px0 = static_cast<float>(ts.min_x) + 0.5f;
        const float row_w0 = ((b.x - a.x) * (py - a.y) - (px0 - a.x) * (b.y - a.y)) * ts.inv_area;
        const float row_w1 = ((px0 - a.x) * (c.y - a.y) - (c.x - a.x) * (py - a.y)) * ts.inv_area;
        const float row_w2 = 1.0f - row_w0 - row_w1;
        float lo = 0.0f, hi = static_cast<float>(ts.max_x - ts.min_x);
        auto bound = [&](float v, float d) {
            if (d > 0.0f) lo = std::max(lo, -v / d);
            else if (d < 0.0f) hi = std::min(hi, -v / d);
            else if (v < 0.0f) hi = -1.0f;
        };
        bound(row_w0, ts.dw0);
        bound(row_w1, ts.dw1);
        bound(row_w2, ts.dw2);
        if (hi < lo) continue;
        const int32_t x_start = ts.min_x + std::max(0, static_cast<int32_t>(std::floor(lo)) - 1);
        const int32_t x_end = std::min(ts.max_x, ts.min_x + static_cast<int32_t>(std::ceil(hi)) + 1);

        const bool fast = row_fast(y, x_start, x_end, Depth);
        const uint32_t crow = color_image_.address + static_cast<uint32_t>(y) * stride * 2u;
        const uint32_t zrow = depth_image_.address + static_cast<uint32_t>(y) * stride * 2u;

        const float w0_start = row_w0 + ts.dw0 * static_cast<float>(x_start - ts.min_x);
        const float w1_start = row_w1 + ts.dw1 * static_cast<float>(x_start - ts.min_x);
        const float ox = static_cast<float>(x_start) + 0.5f - a.x, oy = py - a.y;
        float at[8];
        for (int k = 0; k < 8; ++k) at[k] = ts.base[k] + ts.dx[k] * ox + ts.dy[k] * oy;

        uint32_t row_written = 0;
        for (int32_t cx = x_start; cx <= x_end; cx += SpanChunk) {
            const int n = static_cast<int>(std::min<int32_t>(SpanChunk, x_end - cx + 1));
            const float f0 = static_cast<float>(cx - x_start);
            walked += static_cast<uint64_t>(n);

            // Coverage.
            int live = 0;
            for (int i = 0; i < n; ++i) {
                const float fi = f0 + static_cast<float>(i);
                const float w0 = w0_start + ts.dw0 * fi, w1 = w1_start + ts.dw1 * fi;
                const bool in = w0 >= 0.0f && w1 >= 0.0f && 1.0f - w0 - w1 >= 0.0f;
                sw.live[i] = in ? 1 : 0;
                live += in ? 1 : 0;
            }
            if (live == 0) continue;
            covered += static_cast<uint64_t>(live);

            // Attributes the stages below read.
            auto plane = [&](int k) {
                float* out = sw.attr[k];
                const float base = at[k] + ts.dx[k] * f0, d = ts.dx[k];
                for (int i = 0; i < n; ++i) out[i] = base + d * static_cast<float>(i);
            };
            if constexpr (Textured) {
                plane(0);
                plane(1);
                plane(2);
            }
            if constexpr (Depth) plane(3);
            if (need_shade) {
                plane(4);
                plane(5);
                plane(6);
                plane(7);
            }

            if constexpr (Depth) {
                for (int i = 0; i < n; ++i) {
                    const float z = sw.attr[3][i] < 0.0f ? 0.0f : (sw.attr[3][i] > 1.0f ? 1.0f : sw.attr[3][i]);
                    sw.depth[i] = static_cast<uint16_t>(z * 65535.0f);
                }
                for (int i = 0; i < n; ++i) {
                    if (!sw.live[i]) continue;
                    const int32_t x = cx + i;
                    const uint16_t stored = fast ? rd16(zrow + static_cast<uint32_t>(x) * 2u) : read_depth(x, y);
                    if (sw.depth[i] >= stored) sw.live[i] = 0;
                }
            }

            if constexpr (Textured) {
                if (sp0 != nullptr) {
                    for (int i = 0; i < n; ++i) {
                        if (!sw.live[i]) continue;
                        if (sw.attr[0][i] == 0.0f) {
                            sw.live[i] = 0;
                            continue;
                        }
                        const float scale = ts.persp_scale / sw.attr[0][i];
                        const float s = sw.attr[1][i] * scale, t = sw.attr[2][i] * scale;
                        sw.texel0[i] = sample_fast(ts.tile, *sp0, s, t);
                        // TEXEL1 is the next tile; only a two-cycle combiner reads it.
                        if constexpr (TwoTexels) {
                            if (sp1 != nullptr) sw.texel1[i] = sample_fast(tile1, *sp1, s, t);
                        }
                    }
                    for (int i = 0; i < n; ++i) {
                        const uint32_t v = sw.texel0[i];
                        sw.rows[S_TEXEL0][0][i] = static_cast<int32_t>(v & 0xFFu);
                        sw.rows[S_TEXEL0][1][i] = static_cast<int32_t>((v >> 8) & 0xFFu);
                        sw.rows[S_TEXEL0][2][i] = static_cast<int32_t>((v >> 16) & 0xFFu);
                        sw.rows[S_TEXEL0][3][i] = static_cast<int32_t>(v >> 24);
                    }
                    if (TwoTexels && sp1 != nullptr) {
                        for (int i = 0; i < n; ++i) {
                            const uint32_t v = sw.texel1[i];
                            sw.rows[S_TEXEL1][0][i] = static_cast<int32_t>(v & 0xFFu);
                            sw.rows[S_TEXEL1][1][i] = static_cast<int32_t>((v >> 8) & 0xFFu);
                            sw.rows[S_TEXEL1][2][i] = static_cast<int32_t>((v >> 16) & 0xFFu);
                            sw.rows[S_TEXEL1][3][i] = static_cast<int32_t>(v >> 24);
                        }
                    }
                } else {
                    for (int i = 0; i < n; ++i) {
                        if (sw.live[i] && sw.attr[0][i] == 0.0f) sw.live[i] = 0;
                    }
                }
            }

            if (need_shade) {
                for (int ch = 0; ch < 4; ++ch) {
                    const float* src = sw.attr[4 + ch];
                    int32_t* dst = sw.rows[S_SHADE][ch];
                    for (int i = 0; i < n; ++i) {
                        const float v = src[i] < 0.0f ? 0.0f : (src[i] > 1.0f ? 1.0f : src[i]);
                        dst[i] = static_cast<int32_t>(v * 255.0f + 0.5f);
                    }
                }
            }

            combine_span(n);

            for (int i = 0; i < n; ++i) {
                if (!sw.live[i]) continue;
                const int32_t x = cx + i;
                const uint32_t px = static_cast<uint32_t>(x) * 2u;
                int32_t rgba[4] = { sw.out[0][i], sw.out[1][i], sw.out[2][i], sw.out[3][i] };
                if (ts.alpha_threshold && rgba[3] < threshold) continue;
                if (ts.cvg_x_alpha && rgba[3] <= 48) continue;

                if (!passthrough) {
                    const uint16_t mem = ts.read_fb ? (fast ? rd16(crow + px) : sample(x, y)) : 1u;
                    if (!blend(rgba, mem, sw.rows[S_SHADE][3][i])) continue;
                }

                if (ts.tracing) {
                    if (x == ts.mid_x && y == ts.mid_y) {
                        const uint32_t t0 = Textured ? sw.texel0[i] : 0xFFFFFFFFu;
                        ts.mid_texel = Texel{ static_cast<int32_t>(t0 & 0xFFu), static_cast<int32_t>((t0 >> 8) & 0xFFu),
                                              static_cast<int32_t>((t0 >> 16) & 0xFFu), static_cast<int32_t>(t0 >> 24) };
                    }
                    ++ts.written;
                    ts.sum[0] += rgba[0];
                    ts.sum[1] += rgba[1];
                    ts.sum[2] += rgba[2];
                }

                const uint16_t color = pack16(rgba);
                ++written;
                if (fast) {
                    wr16(crow + px, color);
                    ++row_written;
                    if (Depth && ts.depth_write) wr16(zrow + px, sw.depth[i]);
                } else {
                    plot(x, y, color);
                    if (Depth && ts.depth_write) write_depth(x, y, sw.depth[i]);
                }
            }
        }
        if (fast) note_span(y, x_start, x_end, row_written);
    }
    prof_count_[1] += walked;
    prof_count_[2] += covered;
    prof_count_[3] += written;
    stats_.texels_sampled += covered * ((Textured ? 1u : 0u) + (TwoTexels ? 1u : 0u)) *
                             ((sp0 != nullptr && sp0->bilinear) ? 4u : 1u);
}

void Rdp::triangle(const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c, bool textured,
                   uint32_t tile) {
    ProfScope prof_scope(prof_ms_[2]);
    const float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    if (std::fabs(area) < 1e-6f) {
        ++stats_.triangles_skipped;
        return;
    }

    const int32_t min_x = std::max(clip_x0_, static_cast<int32_t>(std::floor(std::min({ a.x, b.x, c.x }))));
    const int32_t max_x = std::min(clip_x1_ - 1, static_cast<int32_t>(std::ceil(std::max({ a.x, b.x, c.x }))));
    const int32_t min_y = std::max(clip_y0_, static_cast<int32_t>(std::floor(std::min({ a.y, b.y, c.y }))));
    const int32_t max_y = std::min(clip_y1_ - 1, static_cast<int32_t>(std::ceil(std::max({ a.y, b.y, c.y }))));
    if (max_x < min_x || max_y < min_y) {
        ++stats_.triangles_skipped;
        return;
    }
    if (first_row(min_y) > max_y) return;   // none of this worker's rows

    ++stats_.triangles_drawn;
    {
        const Tile& d = tiles_[tile < 8 ? tile : 0];
        const uint32_t prim8 = (to_8bit(prim_color_[0]) << 16) | (to_8bit(prim_color_[1]) << 8) |
                               to_8bit(prim_color_[2]);
        begin_draw(3u, min_x, min_y, max_x, max_y,
                   (d.fmt << 24) | (d.siz << 20) | ((d.line & 0xFFFu) << 8) | (tile & 0x7u) |
                       (textured ? 0x80u : 0u),
                   texture_image_.address, combine_h_ & 0x00FFFFFFu,
                   (((other_mode_.cycle_type() >> OtherMode::G_MDSFT_CYCLETYPE) & 0x3u) << 24) |
                       prim8);
    }

    TriSetup ts;
    ++prof_count_[0];
    ts.a = &a;
    ts.b = &b;
    ts.c = &c;
    ts.min_x = min_x;
    ts.max_x = max_x;
    ts.min_y = min_y;
    ts.max_y = max_y;
    ts.tile = tile;
    ts.inv_area = 1.0f / area;
    ts.persp_scale = (other_mode_.H & (1u << 19)) != 0u ? 1.0f : 0.5f;   // G_TP_PERSP
    ts.alpha_threshold = other_mode_.alpha_compare() == OtherMode::G_AC_THRESHOLD;
    ts.cvg_x_alpha = (other_mode_.L & 0x1000u) != 0u;
    ts.read_fb = blender_reads_framebuffer();
    const bool depth_test = depth_test_enabled();
    ts.depth_write = depth_test && other_mode_.z_update();

    // Barycentrics are linear in x: step them per pixel and walk only the span of
    // each row the triangle covers (solved from the three edge values).
    ts.dw0 = -(b.y - a.y) * ts.inv_area;
    ts.dw1 = (c.y - a.y) * ts.inv_area;
    ts.dw2 = -ts.dw0 - ts.dw1;

    const float va[8] = { a.w, a.s * a.w, a.t * a.w, a.z, a.r, a.g, a.b, a.a };
    const float vb[8] = { b.w, b.s * b.w, b.t * b.w, b.z, b.r, b.g, b.b, b.a };
    const float vc[8] = { c.w, c.s * c.w, c.t * c.w, c.z, c.r, c.g, c.b, c.a };
    for (int k = 0; k < 8; ++k) {
        const float db = vb[k] - va[k], dc = vc[k] - va[k];
        ts.base[k] = va[k];
        ts.dx[k] = (db * (c.y - a.y) - dc * (b.y - a.y)) * ts.inv_area;
        ts.dy[k] = (dc * (b.x - a.x) - db * (c.x - a.x)) * ts.inv_area;
    }

    static const bool tri_trace = std::getenv("WETTER_TRI_TRACE") != nullptr;
    static const bool tri_trace_big = std::getenv("WETTER_TRI_TRACE_BIG") != nullptr;
    const uint32_t tri_bbox_area =
        static_cast<uint32_t>((max_x - min_x + 1) * (max_y - min_y + 1));
    ts.tracing = (tri_trace || capture_armed_) && stats_.frames == traced_frame() &&
                 (!tri_trace_big || capture_armed_ || tri_bbox_area >= 4000u) &&
                 tri_traced_ < 200u;
    if (ts.tracing) ++tri_traced_;
    ts.mid_x = (min_x + max_x) / 2;
    ts.mid_y = (min_y + max_y) / 2;

    // Sample only what the combiner reads.
    if (!cc_.valid) prepare_combiner();
    const bool sampled = textured && (cc_.needs_texel0 || cc_.needs_texel1);
    const bool two = sampled && other_mode_.cycle_type() == OtherMode::G_CYC_2CYCLE && cc_.needs_texel1;
    if (sampled) {
        if (two) depth_test ? raster_triangle<true, true, true>(ts) : raster_triangle<true, true, false>(ts);
        else depth_test ? raster_triangle<true, false, true>(ts) : raster_triangle<true, false, false>(ts);
    } else {
        depth_test ? raster_triangle<false, false, true>(ts) : raster_triangle<false, false, false>(ts);
    }

    if (ts.tracing) {
        const Tile& traced_tile = tiles_[tile < 8 ? tile : 0];
        const double count = (ts.written != 0 ? static_cast<double>(ts.written) : 1.0) * 255.0;
        std::fprintf(trace_out_,
                     "[render] tri %u: bbox (%d,%d)-(%d,%d) %ux%u tile %u textured %u "
                     "tile[fmt %u siz %u line %u] teximg 0x%06X cimg 0x%06X | wrote %u/%u "
                     "avg %.2f %.2f %.2f | mid texel %.2f %.2f %.2f a%.2f | "
                     "st/w a(%.1f %.1f %.5f) b(%.1f %.1f %.5f) c(%.1f %.1f %.5f)\n",
                     tri_traced_, min_x, min_y, max_x, max_y,
                     static_cast<unsigned>(max_x - min_x + 1),
                     static_cast<unsigned>(max_y - min_y + 1), tile, textured ? 1u : 0u,
                     static_cast<unsigned>(traced_tile.fmt), static_cast<unsigned>(traced_tile.siz),
                     static_cast<unsigned>(traced_tile.line), texture_image_.address,
                     color_image_.address, ts.written, tri_bbox_area, ts.sum[0] / count,
                     ts.sum[1] / count, ts.sum[2] / count, ts.mid_texel.r / 255.0,
                     ts.mid_texel.g / 255.0, ts.mid_texel.b / 255.0, ts.mid_texel.a / 255.0,
                     a.s, a.t, a.w, b.s, b.t, b.w, c.s, c.t, c.w);
        std::fflush(trace_out_);
    }
}

void Rdp::compose_frame(uint32_t vi_origin, uint32_t vi_width, uint32_t vi_status, uint32_t vi_v_start,
                        uint32_t vi_y_scale) {
    ++stats_.frames;

    int width = static_cast<int>(vi_width & 0x3FFu);
    if (width <= 0 || width > MaxFramebufferWidth) {
        width = color_image_.width != 0 && color_image_.width <= MaxFramebufferWidth
                    ? static_cast<int>(color_image_.width)
                    : 320;
    }

    // NTSC is 240 lines. V_SYNC would give the exact figure; 240 is what the
    // colour image holds and what the scaler wants.
    int height = static_cast<int>(FramebufferHeight);
    {
        const uint32_t v_start = (vi_v_start >> 16) & 0x3FFu;
        const uint32_t v_end = vi_v_start & 0x3FFu;
        const uint32_t y_scale = vi_y_scale & 0xFFFu;
        if (v_end > v_start && y_scale != 0u) {
            const int lines = static_cast<int>((((v_end - v_start) / 2u) * y_scale) >> 10);
            if (lines > 0 && lines < height) height = lines;
        }
    }
    if (height > MaxFramebufferHeight) height = MaxFramebufferHeight;

    frame_width_ = width;
    frame_height_ = height;

    const uint32_t origin = vi_origin & 0x3FFFFFFu;

    const uint32_t vi_type = vi_status & 0x3u;
    const bool thirty_two_bit = (vi_type == 3u);

    if (vi_log_enabled_) {
        static uint32_t last_origin = 0xFFFFFFFFu;
        static uint32_t last_width = 0xFFFFFFFFu;
        static uint32_t last_status = 0xFFFFFFFFu;
        static unsigned logged = 0;
        if (origin != last_origin || vi_width != last_width || vi_status != last_status ||
            logged < 8) {
            last_origin = origin;
            last_width = vi_width;
            last_status = vi_status;
            ++logged;
            std::fprintf(stderr,
                         "[render] VI frame %llu: ORIGIN 0x%06X WIDTH %u STATUS 0x%08X -> "
                         "%dx%d, last colour image 0x%06X width %u\n",
                         static_cast<unsigned long long>(stats_.frames), origin, vi_width,
                         vi_status, width, height, color_image_.address, color_image_.width);
            std::fflush(stderr);
        }
    }

    for (int y = 0; y < height; ++y) {
        uint8_t* row = frame_.data() + size_t(y) * size_t(MaxFramebufferWidth) * 4u;
        for (int x = 0; x < width; ++x) {
            float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
            if (thirty_two_bit) {
                const uint32_t address = origin + (static_cast<uint32_t>(y) * static_cast<uint32_t>(width) +
                                                  static_cast<uint32_t>(x)) * 4u;
                const uint32_t v = read_word(address);
                r = from_8bit(v >> 24);
                g = from_8bit(v >> 16);
                b = from_8bit(v >> 8);
                a = from_8bit(v);
            } else {
                const uint32_t address = origin + (static_cast<uint32_t>(y) * static_cast<uint32_t>(width) +
                                                  static_cast<uint32_t>(x)) * 2u;
                unpack_rgba16(read_half(address), &r, &g, &b, &a);
            }

            row[size_t(x) * 4u + 0u] = static_cast<uint8_t>(to_8bit(b));
            row[size_t(x) * 4u + 1u] = static_cast<uint8_t>(to_8bit(g));
            row[size_t(x) * 4u + 2u] = static_cast<uint8_t>(to_8bit(r));
            row[size_t(x) * 4u + 3u] = 255;
        }
    }

    if (plots_ != 0 && first_drawn_frame_ == 0) {
        first_drawn_frame_ = stats_.frames;
    }

    if (is_sample_frame()) {
        const uint32_t stride = color_image_.width != 0 ? color_image_.width : 320u;
        const uint32_t picture_end = color_image_.address + stride * FramebufferHeight * 2u;
        if (plots_ == 0) {
            std::fprintf(stderr, "[render] frame %llu: nothing has been plotted\n",
                         static_cast<unsigned long long>(stats_.frames));
        } else {
            std::fprintf(stderr,
                         "[render] frame %llu: %llu plots, wrote 0x%06X..0x%06X, cimg "
                         "0x%06X stride %u ends at 0x%06X, max x %lld max y %lld, %llu "
                         "writes past it\n",
                         static_cast<unsigned long long>(stats_.frames),
                         static_cast<unsigned long long>(plots_), extent_low_, extent_high_,
                         color_image_.address, stride, picture_end,
                         static_cast<long long>(max_x_), static_cast<long long>(max_y_),
                         static_cast<unsigned long long>(outside_writes_));
        }
        std::fflush(stderr);
    }

    dump_frame_if_asked(vi_origin, vi_width, vi_status);
    dump_rdram_if_asked();
    dump_rdram_raw_if_asked();

    // The next frame's draws are numbered from one again, so a serial in a map is
    // a draw of the frame the capture is of rather than of the whole run.
    if (source_map_enabled_) reset_source_map();

    // ... and a scripted run ends here, on a frame number, rather than on a
    // wall-clock timeout that has nothing to do with how far the run got.
    host_frame_composed(stats_.frames);
}

void Rdp::dump_rdram_raw_if_asked() {
    static const char* path = std::getenv("WETTER_RDRAM_RAW");
    if (path == nullptr) return;

    char numbered[1100];
    // WETTER_RDRAM_RAW_AT=<frame> dumps at that frame instead, for a screen the
    // run's own answer would walk past.
    static const char* at = std::getenv("WETTER_RDRAM_RAW_AT");
    if (at != nullptr) {
        if (stats_.frames != std::strtoull(at, nullptr, 10)) return;
        std::snprintf(numbered, sizeof(numbered), "%s", path);
    }
    else if (stats_.frames == traced_frame() - 1u) {
        // The draws of the traced frame run while the counter reads one less than
        // it: the counter is incremented by the compose that follows them.
        std::snprintf(numbered, sizeof(numbered), "%s", path);
    }
    else if (is_sample_frame()) {
        std::snprintf(numbered, sizeof(numbered), "%s.%llu", path,
                      static_cast<unsigned long long>(stats_.frames));
    }
    else {
        return;
    }

    FILE* file = std::fopen(numbered, "wb");
    if (file == nullptr) return;
    const uint32_t length = 4u * 1024u * 1024u;
    std::fwrite(rdram_, 1, length, file);
    std::fclose(file);
    std::fprintf(stderr, "[render] frame %llu: RDRAM bytes 0..0x%X written to %s\n",
                 static_cast<unsigned long long>(stats_.frames), length, numbered);
    std::fflush(stderr);
}

void Rdp::dump_rdram_if_asked() {
    static const char* path = std::getenv("WETTER_RDRAM_DUMP");
    if (path == nullptr) return;
    if (!is_sample_frame()) return;
    if (!color_image_.valid) return;

    char numbered[1024];
    const char* dot = std::strrchr(path, '.');
    if (dot != nullptr && static_cast<size_t>(dot - path) + 32u < sizeof(numbered)) {
        std::snprintf(numbered, sizeof(numbered), "%.*s.%llu%s", static_cast<int>(dot - path),
                      path, static_cast<unsigned long long>(stats_.frames), dot);
    } else {
        std::snprintf(numbered, sizeof(numbered), "%s.%llu", path,
                      static_cast<unsigned long long>(stats_.frames));
    }

    const uint32_t stride = color_image_.width != 0 ? color_image_.width : 320u;
    FILE* file = std::fopen(numbered, "wb");
    if (file == nullptr) return;
    std::fprintf(file, "P6\n%u %u\n255\n", stride, FramebufferHeight);
    for (uint32_t y = 0; y < FramebufferHeight; ++y) {
        for (uint32_t x = 0; x < stride; ++x) {
            float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
            unpack_rgba16(read_half(color_image_.address + (y * stride + x) * 2u), &r, &g, &b, &a);
            std::fputc(static_cast<uint8_t>(to_8bit(r)), file);
            std::fputc(static_cast<uint8_t>(to_8bit(g)), file);
            std::fputc(static_cast<uint8_t>(to_8bit(b)), file);
        }
    }
    std::fclose(file);
    std::fprintf(stderr, "[render] frame %llu: colour image 0x%06X stride %u written to %s\n",
                 static_cast<unsigned long long>(stats_.frames), color_image_.address, stride,
                 numbered);
    std::fflush(stderr);
}

void Rdp::dump_frame_if_asked(uint32_t vi_origin, uint32_t vi_width, uint32_t vi_status) {
    static const char* env_path = std::getenv("WETTER_FRAME_DUMP");

    const bool capturing = capture_armed_ && stats_.frames == capture_frame_;
    const char* path = capturing ? capture_stem_.c_str() : env_path;
    if (path == nullptr) return;

    static const char* at = std::getenv("WETTER_FRAME_DUMP_AT");
    if (capturing) {
        // Deliberately no frame suffix: the file is the one the key promised.
    } else if (at != nullptr && *at != '\0') {
        // Matched as ",<n>," against a comma-wrapped list so that frame 480 is not
        // found inside an entry for 4800.
        char list[512];
        char entry[32];
        std::snprintf(list, sizeof(list), ",%s,", at);
        std::snprintf(entry, sizeof(entry), ",%llu,",
                      static_cast<unsigned long long>(stats_.frames));
        if (std::strstr(list, entry) == nullptr) return;
    } else if (!is_sample_frame()) {
        return;
    }

    char numbered[1024];
    if (capturing) {
        uint32_t hash = 2166136261u;  // FNV-1a over the picture
        const uint8_t* pixels = frame_.data();
        const size_t count =
            static_cast<size_t>(frame_width_) * static_cast<size_t>(frame_height_) * 4u;
        for (size_t i = 0; i < count; ++i) hash = (hash ^ pixels[i]) * 16777619u;

        char stem[1024];
        const char* extension = ".ppm";
        const char* dot = std::strrchr(path, '.');
        if (dot != nullptr && static_cast<size_t>(dot - path) + 32u < sizeof(stem)) {
            std::snprintf(stem, sizeof(stem), "%.*s", static_cast<int>(dot - path), path);
            extension = dot;
        } else {
            std::snprintf(stem, sizeof(stem), "%s", path);
        }
        std::snprintf(numbered, sizeof(numbered), "%s.%08X%s", stem, hash, extension);
        for (unsigned n = 2; n < 100; ++n) {
            std::FILE* probe = std::fopen(numbered, "rb");
            if (probe == nullptr) break;
            std::fclose(probe);
            std::snprintf(numbered, sizeof(numbered), "%s.%08X.%u%s", stem, hash, n, extension);
        }
    } else {
        const char* dot = std::strrchr(path, '.');
        if (dot != nullptr && static_cast<size_t>(dot - path) + 32u < sizeof(numbered)) {
            std::snprintf(numbered, sizeof(numbered), "%.*s.%llu%s", static_cast<int>(dot - path),
                          path, static_cast<unsigned long long>(stats_.frames), dot);
        } else {
            std::snprintf(numbered, sizeof(numbered), "%s.%llu", path,
                          static_cast<unsigned long long>(stats_.frames));
        }
    }

    FILE* file = std::fopen(numbered, "wb");
    if (file == nullptr) return;
    std::fprintf(file, "P6\n%d %d\n255\n", frame_width_, frame_height_);
    for (int y = 0; y < frame_height_; ++y) {
        const uint8_t* row = frame_.data() + size_t(y) * size_t(MaxFramebufferWidth) * 4u;
        for (int x = 0; x < frame_width_; ++x) {
            std::fputc(row[size_t(x) * 4u + 2u], file);  // R
            std::fputc(row[size_t(x) * 4u + 1u], file);  // G
            std::fputc(row[size_t(x) * 4u + 0u], file);  // B
        }
    }
    std::fclose(file);
    std::fprintf(stderr, "[render] frame %llu written to %s (%dx%d)\n",
                 static_cast<unsigned long long>(stats_.frames), numbered, frame_width_,
                 frame_height_);
    std::fflush(stderr);

    if (capturing) {
        char trace_path[1280];
        std::snprintf(trace_path, sizeof(trace_path), "%s.txt", numbered);
        if (std::FILE* out = std::fopen(trace_path, "w")) {
            std::fprintf(out, "# capture %s\n", numbered);
            std::fprintf(out,
                         "# composed frame %llu; its draws ran while the counter read %llu\n",
                         static_cast<unsigned long long>(stats_.frames),
                         static_cast<unsigned long long>(stats_.frames - 1u));
            std::fprintf(out, "# screen %dx%d, colour image 0x%06X width %u, %zu draws\n",
                         frame_width_, frame_height_, color_image_.address, color_image_.width,
                         draws_.size());
            std::fprintf(out, "# VI origin 0x%06X width %u status 0x%08X\n", vi_origin, vi_width,
                         vi_status);
            std::fprintf(out,
                         "# every draw of this frame is listed in %s.map.txt; the lines below\n"
                         "# are a capped sample of them\n", numbered);
            std::fflush(trace_out_);
            std::rewind(trace_out_);
            char buffer[8192];
            size_t got = 0;
            while ((got = std::fread(buffer, 1, sizeof(buffer), trace_out_)) > 0) {
                std::fwrite(buffer, 1, got, out);
            }
            std::fclose(out);
            std::fprintf(stderr, "[render] capture traces written to %s\n", trace_path);
            std::fflush(stderr);
        }
    }

    // The map goes with the picture, under a name derived from it, because it is
    // only meaningful for that frame's draws.
    if (source_map_enabled_) {
        char map_path[1280];
        std::snprintf(map_path, sizeof(map_path), "%s.map.txt", numbered);
        if (std::FILE* map_file = std::fopen(map_path, "w")) {
            dump_source_map(map_file);
            std::fclose(map_file);
            std::fprintf(stderr, "[render] draw map written to %s\n", map_path);
            std::fflush(stderr);
        }
    }

    if (source_map_enabled_) {
        char drawn_path[1280];
        std::snprintf(drawn_path, sizeof(drawn_path), "%s.drawn.ppm", numbered);
        if (std::FILE* drawn = std::fopen(drawn_path, "wb")) {
            const int width = color_image_.width != 0 ? static_cast<int>(color_image_.width) : 320;
            std::fprintf(drawn, "P6\n%d %d\n255\n", width, frame_height_);
            for (int y = 0; y < frame_height_; ++y) {
                for (int x = 0; x < width; ++x) {
                    const uint32_t address = color_image_.address +
                                             (static_cast<uint32_t>(y) * static_cast<uint32_t>(width) +
                                              static_cast<uint32_t>(x)) * 2u;
                    const uint16_t value = read_half(address);
                    std::fputc(static_cast<int>((((value >> 11) & 0x1Fu) << 3) |
                                                (((value >> 11) & 0x1Fu) >> 2)), drawn);
                    std::fputc(static_cast<int>((((value >> 6) & 0x1Fu) << 3) |
                                                (((value >> 6) & 0x1Fu) >> 2)), drawn);
                    std::fputc(static_cast<int>((((value >> 1) & 0x1Fu) << 3) |
                                                (((value >> 1) & 0x1Fu) >> 2)), drawn);
                }
            }
            std::fclose(drawn);
            std::fprintf(stderr, "[render] drawn buffer written to %s\n", drawn_path);
            std::fflush(stderr);
        }
    }

    if (capturing) {
        capture_armed_ = false;
        source_map_enabled_ = std::getenv("WETTER_SOURCE_MAP") != nullptr;
        if (trace_out_ != stderr) {
            std::fclose(trace_out_);
            trace_out_ = stderr;
        }
        std::fprintf(stderr, "[render] capture done: %s (+ .txt and .map.txt)\n", numbered);
        std::fflush(stderr);
    }
}

}  // namespace wetter::soft
