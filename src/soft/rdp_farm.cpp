// soft renderer: the RDP split across threads. See RdpFarm in rdp.h.

#include "rdp.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <immintrin.h>
#endif
#if defined(_WIN32) && defined(__MINGW32__)
#include <pthread.h>
#endif

namespace wetter::soft {

double g_flush_ms = 0.0;
double g_worker_ms[2] = { 0.0, 0.0 };   // slowest, fastest worker per flush, summed
uint64_t g_flushes = 0;

// Workers sleep between sessions. A session starts with the first command after
// a flush; the workers then follow `published` until the flush sets `ending`.
struct RdpFarm::Pool {
    std::mutex mutex;
    std::condition_variable go;
    uint64_t generation = 0;
    bool quit = false;
    std::atomic<size_t> published{ 0 };
    std::atomic<bool> ending{ false };
    std::atomic<size_t> done{ 0 };
    std::vector<std::thread> threads;
};

namespace {

inline void cpu_relax() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#else
    std::this_thread::yield();
#endif
}

size_t worker_count() {
    // Traces, captures and the source map follow one Rdp's view of the frame.
    for (const char* name : { "WETTER_SOURCE_MAP", "WETTER_TEXEL_TRACE", "WETTER_TRI_TRACE",
                              "WETTER_CAPTURE_AT", "WETTER_TRACE_FRAME", "WETTER_FILL_LOG",
                              "WETTER_RECT_LOG" }) {
        if (std::getenv(name) != nullptr) return 1;
    }
    if (const char* v = std::getenv("WETTER_SOFT_THREADS"); v != nullptr && *v != '\0') {
        return std::clamp<size_t>(std::strtoul(v, nullptr, 10), 1, 32);
    }
    // One core stays with the thread recording the display list.
    const unsigned hw = std::thread::hardware_concurrency();
    return std::clamp<size_t>(hw > 1 ? hw - 1 : 1, 1, 7);
}

}  // namespace

RdpFarm::RdpFarm(uint8_t* rdram) {
    const size_t n = worker_count();
    for (size_t i = 0; i < n; ++i) {
        workers_.push_back(new Rdp(rdram));
        workers_.back()->set_rows(static_cast<uint32_t>(i), static_cast<uint32_t>(n));
    }
    active_ = n;
    streaming_ = n > 1;
    if (streaming_) {
        shared_ = new SharedTexels;
        for (Rdp* w : workers_) w->set_shared_texels(shared_);
    }
    replay_ms_.assign(n, 0.0);
    cmds_.resize(MaxCmds);
    tris_.resize(MaxTris);
    pool_ = new Pool;
    if (streaming_) {
        for (size_t i = 0; i < n; ++i) pool_->threads.emplace_back(&RdpFarm::worker_main, this, i);
    }
    std::fprintf(stderr, "[soft] rasterizing on %zu thread%s\n", n, n == 1 ? "" : "s");
    std::fflush(stderr);
}

RdpFarm::~RdpFarm() {
    flush();
    {
        std::lock_guard<std::mutex> lock(pool_->mutex);
        pool_->quit = true;
    }
    pool_->go.notify_all();
    for (auto& t : pool_->threads) t.join();
    delete pool_;
    for (Rdp* w : workers_) delete w;
    delete shared_;
}

void RdpFarm::worker_main(size_t index) {
    Rdp& rdp = *workers_[index];
    uint64_t seen = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(pool_->mutex);
            pool_->go.wait(lock, [&] { return pool_->quit || pool_->generation != seen; });
            if (pool_->quit) return;
            seen = pool_->generation;
        }
        const auto t0 = std::chrono::steady_clock::now();
        size_t at = 0;
        for (;;) {
            const size_t n = pool_->published.load(std::memory_order_acquire);
            while (at < n) apply(rdp, cmds_[at++]);
            if (pool_->ending.load(std::memory_order_acquire)) {
                if (at == pool_->published.load(std::memory_order_acquire)) break;
                continue;
            }
            cpu_relax();
        }
        replay_ms_[index] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        pool_->done.fetch_add(1, std::memory_order_release);
    }
}

void RdpFarm::begin_session() {
    session_ = true;
    shared_->reset();
    for (size_t i = 0; i < active_; ++i) workers_[i]->begin_session();
    {
        std::lock_guard<std::mutex> lock(pool_->mutex);
        ++pool_->generation;
    }
    pool_->go.notify_all();
}

void RdpFarm::record(Op op, std::initializer_list<uint32_t> args) {
    if (count_ == MaxCmds) flush();
    if (streaming_ && !session_) begin_session();
    Cmd& c = cmds_[count_];
    c.op = op;
    size_t i = 0;
    for (uint32_t v : args) c.u[i++] = v;
    ++count_;
    if (streaming_) pool_->published.store(count_, std::memory_order_release);
}

void RdpFarm::apply(Rdp& r, const Cmd& c) const {
    const uint32_t* u = c.u;
    switch (c.op) {
        case Op::ColorImage: r.set_color_image(u[0], u[1], u[2], u[3]); break;
        case Op::DepthImage: r.set_depth_image(u[0]); break;
        case Op::TextureImage: r.set_texture_image(u[0], u[1], u[2], u[3]); break;
        case Op::Tile:
            r.set_tile(u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11]);
            break;
        case Op::TileSize: r.set_tile_size(u[0], u[1], u[2], u[3], u[4]); break;
        case Op::LoadBlock: r.load_block(u[0], u[1], u[2], u[3], u[4]); break;
        case Op::LoadTile: r.load_tile(u[0], u[1], u[2], u[3], u[4]); break;
        case Op::LoadTlut: r.load_tlut(u[0], u[1], u[2], u[3], u[4]); break;
        case Op::Combine: r.set_combine(u[0], u[1]); break;
        case Op::OtherH: r.set_other_mode_h(u[0], u[1], u[2]); break;
        case Op::OtherL: r.set_other_mode_l(u[0], u[1], u[2]); break;
        case Op::Env: r.set_env_color(u[0]); break;
        case Op::Prim: r.set_prim_color(u[0]); break;
        case Op::Blend: r.set_blend_color(u[0]); break;
        case Op::Fog: r.set_fog_color(u[0]); break;
        case Op::Fill: r.set_fill_color(u[0]); break;
        case Op::PrimDepth: r.set_prim_depth(u[0]); break;
        case Op::Scissor: r.set_scissor(u[0], u[1], u[2], u[3]); break;
        case Op::Geometry: r.set_geometry_mode(u[0]); break;
        case Op::FillRect:
            r.fill_rect(static_cast<int32_t>(u[0]), static_cast<int32_t>(u[1]),
                        static_cast<int32_t>(u[2]), static_cast<int32_t>(u[3]));
            break;
        case Op::TexRect:
            r.tex_rect(static_cast<int32_t>(u[0]), static_cast<int32_t>(u[1]), static_cast<int32_t>(u[2]),
                       static_cast<int32_t>(u[3]), u[4], static_cast<int32_t>(u[5]),
                       static_cast<int32_t>(u[6]), static_cast<int32_t>(u[7]),
                       static_cast<int32_t>(u[8]), u[9] != 0u);
            break;
        case Op::Triangle: {
            const TriCmd& t = tris_[u[0]];
            r.triangle(t.v[0], t.v[1], t.v[2], t.textured, t.tile);
            break;
        }
    }
}

void RdpFarm::flush() {
    if (count_ == 0) return;
    ProfScope flush_scope(g_flush_ms);
    ++g_flushes;
    if (!streaming_) {
        for (size_t i = 0; i < count_; ++i) apply(primary(), cmds_[i]);
    } else {
        pool_->ending.store(true, std::memory_order_release);
        while (pool_->done.load(std::memory_order_acquire) != active_) cpu_relax();
        pool_->ending.store(false, std::memory_order_relaxed);
        pool_->done.store(0, std::memory_order_relaxed);
        pool_->published.store(0, std::memory_order_relaxed);
        session_ = false;
        g_worker_ms[0] += *std::max_element(replay_ms_.begin(), replay_ms_.begin() + active_);
        g_worker_ms[1] += *std::min_element(replay_ms_.begin(), replay_ms_.begin() + active_);
        static const bool per_worker = std::getenv("WETTER_WORKER_TIME") != nullptr;
        if (per_worker) {
            static std::vector<double> sum(active_, 0.0);
            static uint32_t flushes = 0;
            for (size_t i = 0; i < active_; ++i) sum[i] += replay_ms_[i];
            if (++flushes == 240) {
                std::fprintf(stderr, "[soft] worker ms per flush:");
                for (size_t i = 0; i < active_; ++i) std::fprintf(stderr, " %.2f", sum[i] / flushes);
                std::fprintf(stderr, "\n");
                std::fflush(stderr);
                std::fill(sum.begin(), sum.end(), 0.0);
                flushes = 0;
            }
        }
    }
    count_ = 0;
    tri_count_ = 0;
    for (size_t i = 0; i < active_; ++i) {
        Rdp& w = *workers_[i];
        for (int k = 0; k < 4; ++k) {
            g_prof_ms[k] += w.prof_ms_[k];
            w.prof_ms_[k] = 0.0;
        }
        for (int k = 0; k < 4; ++k) {
            g_prof_count[k] += w.prof_count_[k];
            w.prof_count_[k] = 0;
        }
    }
}

void* RdpFarm::worker_handle(size_t i) const {
#if defined(_WIN32) && defined(__MINGW32__)
    if (i < pool_->threads.size()) {
        return pthread_gethandle(const_cast<std::thread&>(pool_->threads[i]).native_handle());
    }
#else
    (void)i;
#endif
    return nullptr;
}

void RdpFarm::go_single() {
    flush();
    for (Rdp* w : workers_) {
        w->set_shared_texels(nullptr);
        w->begin_session();
    }
    streaming_ = false;
    active_ = 1;
    primary().set_rows(0, 1);
}


void RdpFarm::set_color_image(uint32_t fmt, uint32_t siz, uint32_t width, uint32_t address) {
    // Whatever was drawn to the previous image may be loaded as a texture next.
    flush();
    record(Op::ColorImage, { fmt, siz, width, address });
}
void RdpFarm::set_depth_image(uint32_t address) { record(Op::DepthImage, { address }); }
void RdpFarm::set_texture_image(uint32_t fmt, uint32_t siz, uint32_t width, uint32_t address) {
    record(Op::TextureImage, { fmt, siz, width, address });
}
void RdpFarm::set_tile(uint32_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint32_t tile,
                       uint32_t palette, uint32_t cm_t, uint32_t mask_t, uint32_t shift_t,
                       uint32_t cm_s, uint32_t mask_s, uint32_t shift_s) {
    record(Op::Tile, { fmt, siz, line, tmem, tile, palette, cm_t, mask_t, shift_t, cm_s, mask_s, shift_s });
}
void RdpFarm::set_tile_size(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    record(Op::TileSize, { tile, uls, ult, lrs, lrt });
}
void RdpFarm::load_block(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) {
    record(Op::LoadBlock, { tile, uls, ult, lrs, dxt });
}
void RdpFarm::load_tile(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    record(Op::LoadTile, { tile, uls, ult, lrs, lrt });
}
void RdpFarm::load_tlut(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    record(Op::LoadTlut, { tile, uls, ult, lrs, lrt });
}
void RdpFarm::set_combine(uint32_t w0, uint32_t w1) { record(Op::Combine, { w0, w1 }); }
void RdpFarm::set_other_mode_h(uint32_t shift, uint32_t length, uint32_t data) {
    record(Op::OtherH, { shift, length, data });
}
void RdpFarm::set_other_mode_l(uint32_t shift, uint32_t length, uint32_t data) {
    record(Op::OtherL, { shift, length, data });
}
void RdpFarm::set_env_color(uint32_t rgba) { record(Op::Env, { rgba }); }
void RdpFarm::set_prim_color(uint32_t rgba) { record(Op::Prim, { rgba }); }
void RdpFarm::set_blend_color(uint32_t rgba) { record(Op::Blend, { rgba }); }
void RdpFarm::set_fog_color(uint32_t rgba) { record(Op::Fog, { rgba }); }
void RdpFarm::set_fill_color(uint32_t rgba) { record(Op::Fill, { rgba }); }
void RdpFarm::set_prim_depth(uint32_t value) { record(Op::PrimDepth, { value }); }
void RdpFarm::set_scissor(uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    record(Op::Scissor, { ulx, uly, lrx, lry });
}
void RdpFarm::set_geometry_mode(uint32_t mode) { record(Op::Geometry, { mode }); }
void RdpFarm::fill_rect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    record(Op::FillRect, { static_cast<uint32_t>(ulx), static_cast<uint32_t>(uly),
                           static_cast<uint32_t>(lrx), static_cast<uint32_t>(lry) });
}
void RdpFarm::tex_rect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint32_t tile, int32_t uls,
                       int32_t ult, int32_t dsdx, int32_t dtdy, bool flip) {
    record(Op::TexRect, { static_cast<uint32_t>(ulx), static_cast<uint32_t>(uly), static_cast<uint32_t>(lrx),
                          static_cast<uint32_t>(lry), tile, static_cast<uint32_t>(uls),
                          static_cast<uint32_t>(ult), static_cast<uint32_t>(dsdx),
                          static_cast<uint32_t>(dtdy), flip ? 1u : 0u });
}
void RdpFarm::triangle(const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c, bool textured,
                       uint32_t tile) {
    if (tri_count_ == MaxTris || count_ == MaxCmds) flush();
    tris_[tri_count_] = TriCmd{ { a, b, c }, textured, tile };
    record(Op::Triangle, { static_cast<uint32_t>(tri_count_++) });
}

void RdpFarm::log_texgen_draw(const ScreenVertex* corners, uint32_t tile_index) {
    flush();
    primary().log_texgen_draw(corners, tile_index);
}
void RdpFarm::log_flat_draw(const ScreenVertex* corners, uint32_t tile_index, uint32_t geometry_mode,
                            uint32_t area) {
    flush();
    primary().log_flat_draw(corners, tile_index, geometry_mode, area);
}

void RdpFarm::compose_frame(uint32_t vi_origin, uint32_t vi_width, uint32_t vi_status, uint32_t vi_v_start,
                            uint32_t vi_y_scale) {
    flush();
    primary().compose_frame(vi_origin, vi_width, vi_status, vi_v_start, vi_y_scale);
}

void RdpFarm::arm_capture(uint64_t draw_frame, const std::string& ppm_path) {
    go_single();
    primary().arm_capture(draw_frame, ppm_path);
}

const Rdp::Stats& RdpFarm::stats() {
    flush();   // the workers' counters are theirs while a session runs
    stats_ = primary().stats();
    for (size_t i = 1; i < active_; ++i) {
        stats_.pixels_drawn += workers_[i]->stats().pixels_drawn;
        stats_.texels_sampled += workers_[i]->stats().texels_sampled;
    }
    return stats_;
}

}  // namespace wetter::soft
