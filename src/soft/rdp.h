// soft renderer: the RDP -- TMEM, texture sampling, combiner, blender, depth,
// rasterizer. Draws into the game's framebuffer in RDRAM.

#ifndef WETTER_SOFT_RDP_H
#define WETTER_SOFT_RDP_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

#include "front/rdp_sink.h"
#include "front/tmem.h"

namespace wetter::soft {

// WETTER_RENDER_TIME profiling: milliseconds spent per primitive kind
// (0 fill, 1 texrect, 2 triangle), accumulated until the caller reads them.
extern double g_prof_ms[4];
// triangles, pixels walked, pixels covered, pixels written
extern uint64_t g_prof_count[4];
// wall time inside RdpFarm::flush, and how many flushes
extern double g_flush_ms;
extern uint64_t g_flushes;
extern double g_worker_ms[2];
struct ProfScope {
    double& slot;
    std::chrono::steady_clock::time_point t0;
    explicit ProfScope(double& s) : slot(s), t0(std::chrono::steady_clock::now()) {}
    ~ProfScope() {
        slot += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
};


using front::RdramSize;
using front::ScreenVertex;

constexpr int MaxFramebufferWidth = 640;
constexpr int MaxFramebufferHeight = 480;

constexpr uint32_t FramebufferHeight = 240;

inline int32_t fixed_to_pixel_floor(uint32_t value) { return static_cast<int32_t>(value >> 2); }
inline int32_t fixed_to_pixel_ceil(uint32_t value) {
    return static_cast<int32_t>((value + 3u) >> 2);
}

struct OtherMode {
    uint32_t L = 0;  // composed from G_SETOTHERMODE_L
    uint32_t H = 0;  // composed from G_SETOTHERMODE_H

    static constexpr uint32_t G_MDSFT_ALPHACOMPARE = 0;
    static constexpr uint32_t G_MDSFT_ZSRCSEL = 2;
    static constexpr uint32_t G_MDSFT_COMBKEY = 8;
    static constexpr uint32_t G_MDSFT_TEXTFILT = 12;
    static constexpr uint32_t G_MDSFT_TEXTLUT = 14;
    static constexpr uint32_t G_MDSFT_TEXTLOD = 16;
    static constexpr uint32_t G_MDSFT_TEXTDETAIL = 17;
    static constexpr uint32_t G_MDSFT_TEXTPERSP = 19;
    static constexpr uint32_t G_MDSFT_CYCLETYPE = 20;

    static constexpr uint32_t G_CYC_1CYCLE = 0u << G_MDSFT_CYCLETYPE;
    static constexpr uint32_t G_CYC_2CYCLE = 1u << G_MDSFT_CYCLETYPE;
    static constexpr uint32_t G_CYC_COPY = 2u << G_MDSFT_CYCLETYPE;
    static constexpr uint32_t G_CYC_FILL = 3u << G_MDSFT_CYCLETYPE;

    static constexpr uint32_t AA_EN = 0x8;
    static constexpr uint32_t Z_CMP = 0x10;
    static constexpr uint32_t Z_UPD = 0x20;
    static constexpr uint32_t IM_RD = 0x40;
    static constexpr uint32_t CLR_ON_CVG = 0x80;
    static constexpr uint32_t ZMODE_MASK = 0xC00;
    static constexpr uint32_t ZMODE_OPA = 0x000;
    static constexpr uint32_t ZMODE_INTER = 0x400;
    static constexpr uint32_t ZMODE_XLU = 0x800;
    static constexpr uint32_t ZMODE_DEC = 0xC00;
    static constexpr uint32_t FORCE_BL = 0x4000;

    static constexpr uint32_t G_AC_NONE = 0u << G_MDSFT_ALPHACOMPARE;
    static constexpr uint32_t G_AC_THRESHOLD = 1u << G_MDSFT_ALPHACOMPARE;
    static constexpr uint32_t G_AC_DITHER = 3u << G_MDSFT_ALPHACOMPARE;

    static constexpr uint32_t G_TT_NONE = 0u << G_MDSFT_TEXTLUT;
    static constexpr uint32_t G_TT_RGBA16 = 2u << G_MDSFT_TEXTLUT;
    static constexpr uint32_t G_TT_IA16 = 3u << G_MDSFT_TEXTLUT;

    static constexpr uint32_t G_TF_POINT = 0u << G_MDSFT_TEXTFILT;
    static constexpr uint32_t G_TF_BILERP = 2u << G_MDSFT_TEXTFILT;
    static constexpr uint32_t G_TF_AVERAGE = 3u << G_MDSFT_TEXTFILT;

    static constexpr uint32_t G_TP_NONE = 0u << G_MDSFT_TEXTPERSP;
    static constexpr uint32_t G_TP_PERSP = 1u << G_MDSFT_TEXTPERSP;

    uint32_t cycle_type() const { return H & (3u << G_MDSFT_CYCLETYPE); }
    uint32_t alpha_compare() const { return L & (3u << G_MDSFT_ALPHACOMPARE); }
    uint32_t blender_inputs() const { return (L >> 16) & 0xFFFFu; }
    uint32_t texture_lut() const { return H & (3u << G_MDSFT_TEXTLUT); }
    uint32_t texture_filter() const { return H & (3u << G_MDSFT_TEXTFILT); }
    uint32_t texture_persp() const { return H & (1u << G_MDSFT_TEXTPERSP); }
    bool force_blend() const { return (L & FORCE_BL) != 0; }
    bool z_compare() const { return (L & Z_CMP) != 0; }
    bool z_update() const { return (L & Z_UPD) != 0; }
};


// Decoded textures shared by the RdpFarm workers. A texture is known by the
// count of sampler invalidations before it and its tile; every worker replays
// the same commands, so all of them name it alike. The first to need it
// decodes it here; the table is emptied at the start of every session.
struct SharedTexels {
    static constexpr size_t Slots = size_t(1) << 15;
    static constexpr size_t ArenaTexels = size_t(1) << 22;
    std::atomic<uint8_t> state[Slots];   // 0 empty, 1 decoding, 2 ready, 3 not shared
    uint32_t* data[Slots];
    std::vector<uint32_t> arena;
    std::atomic<size_t> used{ 0 };
    SharedTexels() : arena(ArenaTexels) { reset(); }
    void reset() {
        for (auto& s : state) s.store(0, std::memory_order_relaxed);
        used.store(0, std::memory_order_relaxed);
    }
};

class Rdp {
public:
    explicit Rdp(uint8_t* rdram);

    void set_color_image(uint32_t fmt, uint32_t siz, uint32_t width, uint32_t address);
    void set_depth_image(uint32_t address);
    void set_texture_image(uint32_t fmt, uint32_t siz, uint32_t width, uint32_t address);

    void set_tile(uint32_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint32_t tile,
                  uint32_t palette, uint32_t cm_t, uint32_t mask_t, uint32_t shift_t,
                  uint32_t cm_s, uint32_t mask_s, uint32_t shift_s);
    void set_tile_size(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt);

    void load_block(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt);
    void load_tile(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt);
    void load_tlut(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt);

    void log_tile_load(const char* which, const char* extent_name, uint32_t tile, uint32_t uls,
                       uint32_t ult, uint32_t lrs, uint32_t extent);

    uint64_t frames() const { return stats_.frames; }

    uint64_t traced_frame() const;

    bool is_sample_frame() const;

    uint32_t last_sample_s() const { return last_sample_s_; }
    uint32_t last_sample_t() const { return last_sample_t_; }

    void set_combine(uint32_t w0, uint32_t w1);

    // G_SETOTHERMODE_H/L carry a shift and a length; the composed words are what
    // every field accessor above reads.
    void set_other_mode_h(uint32_t shift, uint32_t length, uint32_t data);
    void set_other_mode_l(uint32_t shift, uint32_t length, uint32_t data);

    void set_env_color(uint32_t rgba);
    void set_prim_color(uint32_t rgba);
    void set_blend_color(uint32_t rgba);
    void set_fog_color(uint32_t rgba);
    void set_fill_color(uint32_t rgba);
    void set_prim_depth(uint32_t value);

    void set_scissor(uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry);

    // From G_SETGEOMETRYMODE / G_CLEARGEOMETRYMODE. Only the bits the rasterizer
    // acts on are consulted: z-buffering, culling, texture enable, shading.
    void set_geometry_mode(uint32_t mode);

    void log_texgen_draw(const ScreenVertex* corners, uint32_t tile_index);

    void log_flat_draw(const ScreenVertex* corners, uint32_t tile_index, uint32_t geometry_mode,
                       uint32_t area);

    void fill_rect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry);

    void tex_rect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint32_t tile, int32_t uls,
                  int32_t ult, int32_t dsdx, int32_t dtdy, bool flip);
    void triangle(const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c,
                  bool textured, uint32_t tile);

    void compose_frame(uint32_t vi_origin, uint32_t vi_width, uint32_t vi_status,
                       uint32_t vi_v_start = 0, uint32_t vi_y_scale = 0);
    const uint8_t* frame_pixels() const { return frame_.data(); }
    int frame_width() const { return frame_width_; }
    int frame_height() const { return frame_height_; }

    struct Stats {
        uint64_t fill_rects = 0;
        uint64_t tex_rects = 0;
        uint64_t triangles_drawn = 0;
        uint64_t triangles_skipped = 0;   // degenerate or fully clipped
        uint64_t pixels_drawn = 0;
        uint64_t texels_sampled = 0;
        uint64_t frames = 0;
        uint64_t unsupported_texture_format = 0;
    };
    const Stats& stats() const { return stats_; }
    void reset_stats() { stats_ = Stats{}; }

    void arm_capture(uint64_t draw_frame, const std::string& ppm_path);

    // Draw only rows y with y % n == k (one worker of RdpFarm).
    void set_rdram(uint8_t* rdram) {
        rdram_ = rdram;
        tm_.set_rdram(rdram);
    }
    // Share decoded textures through `shared` (null: decode alone).
    void set_shared_texels(SharedTexels* shared) { shared_ = shared; }
    // A new session: the shared table is empty, so nothing prepared stands.
    void begin_session() {
        for (auto& s : samplers_) s.valid = false;
        sampler_epoch_ = 0;
    }

    void set_rows(uint32_t k, uint32_t n) {
        row_k_ = static_cast<int32_t>(k);
        row_n_ = static_cast<int32_t>(n);
        const char* band = std::getenv("WETTER_SOFT_BAND");
        row_band_ = band != nullptr ? std::max(1, std::atoi(band)) : 16;
    }
    // Per-instance WETTER_RENDER_TIME counters; RdpFarm folds them into g_prof_*.
    double prof_ms_[4] = { 0.0, 0.0, 0.0, 0.0 };
    uint64_t prof_count_[4] = { 0, 0, 0, 0 };

private:
    // Rows are dealt out in bands (WETTER_SOFT_BAND, default 16): a small triangle
    // then touches one or two workers instead of all of them.
    int32_t row_k_ = 0, row_n_ = 1, row_band_ = 1;
    SharedTexels* shared_ = nullptr;
    uint32_t sampler_epoch_ = 0;
    // This worker's first row at or below y0, and the one after y.
    int32_t first_row(int32_t y0) const {
        const int32_t band = y0 >= 0 ? y0 / row_band_ : -((-y0 + row_band_ - 1) / row_band_);
        int32_t r = (row_k_ - band) % row_n_;
        if (r < 0) r += row_n_;
        return r == 0 ? y0 : (band + r) * row_band_;
    }
    int32_t next_row(int32_t y) const {
        ++y;
        return (y % row_band_ != 0 || row_n_ == 1) ? y : y + (row_n_ - 1) * row_band_;
    }

    // Texture memory lives in the shared front::Tmem (see the members below).
    using Tile = front::TileDesc;
    using TextureImage = front::ImageDesc;

    uint8_t read_byte(uint32_t address) const;
    uint16_t read_half(uint32_t address) const;
    uint32_t read_word(uint32_t address) const;
    void write_half(uint32_t address, uint16_t value);

    bool framebuffer_contains(uint32_t address) const;

    // --- framebuffer --------------------------------------------------------
    // The colour image, RGBA16 in RDRAM. Every primitive writes here.
    void plot(int32_t x, int32_t y, uint16_t color);
    uint16_t sample(int32_t x, int32_t y) const;

    bool depth_test_enabled() const;
    uint16_t read_depth(int32_t x, int32_t y) const;
    void write_depth(int32_t x, int32_t y, uint16_t value);
    static uint16_t pack_rgba16(float r, float g, float b, float a);
    static void unpack_rgba16(uint16_t value, float* r, float* g, float* b, float* a);

    // --- texture sampling ---------------------------------------------------
    // Channels are 8-bit, 0..255, as the RDP carries them.
    struct Texel {
        int32_t r = 255, g = 255, b = 255, a = 255;
    };
    Texel sample_tile(uint32_t tile_index, float s, float t);
    uint32_t fetch_texel(uint32_t tile_index, uint32_t s, uint32_t t);   // packed RGBA8, s/t addressed
    static uint32_t pack_texel(const Texel& t) {
        return static_cast<uint32_t>(t.r) | (static_cast<uint32_t>(t.g) << 8) |
               (static_cast<uint32_t>(t.b) << 16) | (static_cast<uint32_t>(t.a) << 24);
    }
    Texel decode_texel(const Tile& tile, uint32_t s, uint32_t t);

    static uint32_t tile_texel_width(const Tile& tile) { return ((tile.lrs - tile.uls) >> 2) + 1u; }
    static uint32_t tile_texel_height(const Tile& tile) { return ((tile.lrt - tile.ult) >> 2) + 1u; }

    // --- shading ------------------------------------------------------------
    // One pixel through the combiner: two texels and the shade in, RGBA out.
    // Texels packed RGBA8 (r in the low byte).
    void combine(uint32_t texel0, uint32_t texel1, const int32_t* shade, int32_t* out);

    // Decoded combiner/blender state, rebuilt only when the combine word, other
    // modes or constant colours change (every setter clears `valid`).
    struct CombinerSetup {
        bool valid = false;
        bool compiled = false;   // the tables below; colours are refreshed apart
        int32_t base[16][4];     // constant operand rows (prim, env, one, zero)
        int32_t src[16][4];      // working rows: the constants plus this pixel's inputs
        uint8_t sel[2][8];       // operand sources per cycle: rgb a b c d, alpha a b c d
        uint8_t sel_second[8];   // cycle 1 of a two-cycle combine, TEXEL0/TEXEL1 swapped
        uint32_t cycle = 0;
        int32_t fill[4];
        bool needs_texel0 = true, needs_texel1 = true;   // any operand reads that row
        bool needs_shade = true;
        // Each cycle compiled: per channel, the four operands as offsets into
        // src, and the form: 0 general, 1 D alone, 2 A*C. Slot 0 is a two-cycle
        // combine's first cycle, slot 1 the last (or only) cycle.
        uint16_t off[2][4][4];
        uint8_t form[2][4];
        const int32_t* optr[2][4][4];   // the same operands as rows of span_
    };
    struct BlenderSetup {
        bool valid = false;
        uint8_t sel[2][4];       // p, a, m, b per cycle
        bool reads_framebuffer = false;
        bool passthrough = false;   // copy and fill have no blender
        bool force = false;
        int cycles = 1;
        int32_t blend[4];
        int32_t fog[4];
        bool uses_shade_alpha = false;
    };
    CombinerSetup cc_;
    BlenderSetup bl_;
    // Per-tile sampling parameters, derived from the tile descriptor and the
    // filter mode; rebuilt after any tile, tile-size, load or other-mode change.
    struct SamplerSetup {
        bool valid = false;
        float scale_s = 1.0f, scale_t = 1.0f;   // the tile shift
        float offset_s = 0.0f, offset_t = 0.0f; // sl/tl in texels
        int32_t width = 1, height = 1;
        bool clamp_s = false, clamp_t = false, wrap_s = false, wrap_t = false;
        bool mirror_s = false, mirror_t = false, bilinear = false;
        // Decoded texels over the addressable range, filled on first use and
        // tagged with the generation that decoded them.
        bool cached = false;
        uint32_t cache_w = 0, cache_h = 0;
        uint32_t gen = 0;
        std::vector<uint32_t> cache;
        std::vector<uint32_t> cache_gen;
        bool complete = false;   // every texel decoded: no generation checks
        const uint32_t* texels = nullptr;   // when complete: the cache, or a shared copy
        // Addressing leaves an index alone below these; and s/t to 8-bit-fraction texels.
        uint32_t inner_w = 0, inner_h = 0;
        float mul_s = 256.0f, add_s = 0.0f, mul_t = 256.0f, add_t = 0.0f;
    };
    SamplerSetup samplers_[8];
    void prepare_sampler(uint32_t tile);

    // --- spans ----------------------------------------------------------------
    // A triangle row is shaded SpanChunk pixels at a time, one stage after
    // another over arrays: coverage and attributes, depth, texels, shade, the
    // combiner, then tests, blending and the store.
    static constexpr int SpanChunk = 64;
    struct alignas(64) SpanWork {
        int32_t rows[16][4][SpanChunk];   // combiner operands by source and channel
        int32_t out[4][SpanChunk];
        float attr[8][SpanChunk];
        uint32_t texel0[SpanChunk], texel1[SpanChunk];
        uint16_t depth[SpanChunk];
        uint8_t live[SpanChunk];
    };
    SpanWork span_;
    bool span_constants_stale_ = true;   // the constant rows lag the combiner
    bool span_white_[16] = {};           // per-pixel row holding 255 throughout
    int32_t span_const_[16][4];          // what each constant row holds (-1: unknown)
    void fill_span_constants();
    void fill_span_row(int row, int32_t value);
    void combine_span(int n);
    // sample_tile without the per-call checks: the caller has prepared `sp`.
    uint32_t sample_fast(uint32_t tile_index, SamplerSetup& sp, float s, float t);
    void prepare_combiner();
    void prepare_blender();
    void invalidate_setup() {
        cc_.valid = false;
        cc_.compiled = false;
        bl_.valid = false;
        invalidate_samplers();
    }
    void invalidate_combiner() {
        cc_.valid = false;
        cc_.compiled = false;
    }
    void invalidate_combiner_colors() { cc_.valid = false; }
    void invalidate_blender() { bl_.valid = false; }
    void invalidate_samplers() {
        for (auto& s : samplers_) s.valid = false;
        ++sampler_epoch_;
    }
    bool blender_reads_framebuffer() {
        if (!bl_.valid) prepare_blender();
        return bl_.reads_framebuffer;
    }

    // The blender; see rdp.cpp. `shade_alpha` is the blender's A input 2. False
    // when the pixel keeps what memory holds.
    bool blend(int32_t* rgba, uint16_t framebuffer, int32_t shade_alpha);

    // --- span access --------------------------------------------------------
    // Rows that lie wholly inside the scissor, the colour image and RDRAM are
    // drawn straight into memory; anything else goes through plot().
    bool row_fast(int32_t y, int32_t x0, int32_t x1, bool depth) const;
    uint16_t rd16(uint32_t address) const;
    void wr16(uint32_t address, uint16_t value);
    void note_span(int32_t y, int32_t x0, int32_t x1, uint32_t written);

    struct TriSetup;
    template <bool Textured, bool TwoTexels, bool Depth>
    void raster_triangle(TriSetup& ts);
    bool no_plot_ = false;

    // --- scissor ------------------------------------------------------------
    int32_t clip_x0_ = 0, clip_y0_ = 0, clip_x1_ = 0, clip_y1_ = 0;

    struct DrawRecord {
        uint32_t kind = 0;  // 1 fill, 2 texrect, 3 triangle
        int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        // Kind-specific, printed by dump_source_map: a fill's colour, a
        // rectangle's tile and texture address, a triangle's tile and combiner.
        uint32_t v0 = 0, v1 = 0, v2 = 0, v3 = 0;
    };
    std::vector<DrawRecord> draws_;
    std::vector<uint32_t> pixel_source_;
    uint32_t draw_serial_ = 0;

    // Numbers one draw and returns its serial, or 0 when the map is off. Called
    // at the top of every primitive, whether or not it ends up writing anything.
    uint32_t begin_draw(uint32_t kind, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t v0,
                        uint32_t v1, uint32_t v2, uint32_t v3);
    void reset_source_map();
    void dump_source_map(std::FILE* out);

    static constexpr uint32_t RectLogCount = 12;
    static constexpr uint32_t TexRectLogCount = 300;
    uint32_t tex_rect_log_count_ = 0;
    uint32_t traced_ = 0;
    uint32_t tri_traced_ = 0;
    bool rect_log_enabled_ = false;

    uint64_t trace_frame_ = 0;
    uint64_t capture_frame_ = 0;
    bool capture_armed_ = false;
    bool source_map_enabled_ = false;
    std::string capture_stem_;
    std::FILE* trace_out_ = stderr;
    uint32_t last_sample_s_ = 0;
    uint32_t last_sample_t_ = 0;

    bool cimg_log_enabled_ = false;
    uint32_t cimg_log_count_ = 0;

    bool extent_reported_ = false;
    uint32_t extent_low_ = 0xFFFFFFFFu;
    uint32_t extent_high_ = 0;
    int64_t max_y_ = -1;
    int64_t max_x_ = -1;
    uint64_t outside_writes_ = 0;
    uint64_t plots_ = 0;
    uint64_t first_drawn_frame_ = 0;

    uint8_t* rdram_ = nullptr;

    front::Tmem tm_;
    TextureImage& texture_image_ = tm_.image;
    TextureImage color_image_;
    TextureImage depth_image_;
    Tile (&tiles_)[8] = tm_.tiles;
    uint8_t (&tmem_)[4096] = tm_.bytes;
    uint32_t& palette_address_ = tm_.palette_address;
    uint32_t& palette_count_ = tm_.palette_count;

    OtherMode other_mode_;
    uint32_t combine_l_ = 0;   // the combine word's low 32 bits: w0 & 0xFFFFFF
    uint32_t combine_h_ = 0;   // ... and its high 32: w1
    uint32_t geometry_mode_ = 0;
    float env_color_[4] = { 0, 0, 0, 0 };
    float prim_color_[4] = { 0, 0, 0, 0 };
    float blend_color_[4] = { 0, 0, 0, 0 };
    float fog_color_[4] = { 0, 0, 0, 0 };
    float fill_color_[4] = { 0, 0, 0, 0 };
    uint32_t fill_raw_ = 0;
    float prim_depth_ = 0.0f;

    void dump_frame_if_asked(uint32_t vi_origin, uint32_t vi_width, uint32_t vi_status);

    void dump_rdram_if_asked();

    void dump_rdram_raw_if_asked();

    bool vi_log_enabled_ = false;

    std::vector<uint8_t> frame_;   // ARGB8888 staging buffer
    int frame_width_ = 0;
    int frame_height_ = 0;

    Stats stats_{};
};

// The RDP split across threads. Every command is recorded and replayed, in
// order, by each worker's own Rdp, so all of them hold the same state; each
// draws only its share of the rows. flush() runs the recording and waits.
// Changing the colour image flushes first, so a later texture load reads a
// finished picture. Debug captures and traces run on the primary alone.
class RdpFarm final : public front::RdpSink {
public:
    explicit RdpFarm(uint8_t* rdram);
    ~RdpFarm();
    RdpFarm(const RdpFarm&) = delete;
    RdpFarm& operator=(const RdpFarm&) = delete;

    void set_color_image(uint32_t fmt, uint32_t siz, uint32_t width, uint32_t address) override;
    void set_depth_image(uint32_t address) override;
    void set_texture_image(uint32_t fmt, uint32_t siz, uint32_t width, uint32_t address) override;
    void set_tile(uint32_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint32_t tile,
                  uint32_t palette, uint32_t cm_t, uint32_t mask_t, uint32_t shift_t,
                  uint32_t cm_s, uint32_t mask_s, uint32_t shift_s) override;
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
    void tex_rect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint32_t tile, int32_t uls,
                  int32_t ult, int32_t dsdx, int32_t dtdy, bool flip) override;
    void triangle(const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c,
                  bool textured, uint32_t tile) override;

    void log_texgen_draw(const ScreenVertex* corners, uint32_t tile_index) override;
    void log_flat_draw(const ScreenVertex* corners, uint32_t tile_index, uint32_t geometry_mode,
                       uint32_t area) override;

    void flush();
    // The native handle of worker thread i (for the sampler), or null.
    void* worker_handle(size_t i) const;
    void set_rdram(uint8_t* rdram) {
        flush();
        for (Rdp* w : workers_) w->set_rdram(rdram);
    }
    void compose_frame(uint32_t vi_origin, uint32_t vi_width, uint32_t vi_status,
                       uint32_t vi_v_start = 0, uint32_t vi_y_scale = 0);
    void arm_capture(uint64_t draw_frame, const std::string& ppm_path);

    uint64_t frames() const override { return primary().frames(); }
    uint64_t traced_frame() const override { return primary().traced_frame(); }
    const uint8_t* frame_pixels() const { return primary().frame_pixels(); }
    int frame_width() const { return primary().frame_width(); }
    int frame_height() const { return primary().frame_height(); }
    const Rdp::Stats& stats();

private:
    enum class Op : uint8_t {
        ColorImage, DepthImage, TextureImage, Tile, TileSize, LoadBlock, LoadTile, LoadTlut,
        Combine, OtherH, OtherL, Env, Prim, Blend, Fog, Fill, PrimDepth, Scissor, Geometry,
        FillRect, TexRect, Triangle
    };
    struct Cmd {
        Op op;
        uint32_t u[12];
    };
    struct TriCmd {
        ScreenVertex v[3];
        bool textured;
        uint32_t tile;
    };

    Rdp& primary() { return *workers_[0]; }
    const Rdp& primary() const { return *workers_[0]; }
    void record(Op op, std::initializer_list<uint32_t> args);
    void apply(Rdp& rdp, const Cmd& c) const;
    void begin_session();
    void go_single();
    void worker_main(size_t index);

    // Workers read commands while they are still being recorded, so the
    // buffers never move; a full buffer is flushed.
    static constexpr size_t MaxCmds = size_t(1) << 16;
    static constexpr size_t MaxTris = size_t(1) << 15;

    struct Pool;
    std::vector<Rdp*> workers_;
    size_t active_ = 1;
    bool streaming_ = false;   // worker threads draw; otherwise the caller replays
    bool session_ = false;     // workers are awake and following the recording
    std::vector<Cmd> cmds_;
    std::vector<TriCmd> tris_;
    size_t count_ = 0, tri_count_ = 0;
    Pool* pool_ = nullptr;
    std::vector<double> replay_ms_;
    SharedTexels* shared_ = nullptr;
    Rdp::Stats stats_{};
};

}  // namespace wetter::soft

#endif  // WETTER_SOFT_RDP_H
