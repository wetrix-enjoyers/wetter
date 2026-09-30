// soft: the Renderer. Walks F3DEX display lists out of RDRAM and drives the
// Rsp and the RDP (an RdpFarm of worker Rdps).

#include <wetter/wetter.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "front/frontend.h"
#include "front/gbi.h"
#include "hooks.h"
#include "rdp.h"

namespace wetter::soft {

namespace {

constexpr uint32_t AddressMask = 0x3FFFFFFu;

// How often the running total is printed while the game plays. Behind
// WETTER_RENDER_STATS, so an ordinary run says nothing.
constexpr uint32_t DisplayListsPerReport = 600;

class SoftRenderer final : public Renderer {
public:
    SoftRenderer(uint8_t* rdram, const Options& options)
        : rdram_(rdram),
          rdp_(rdram),
          front_(rdram, rdp_, options.force_branch_z),
          stats_enabled_(std::getenv("WETTER_RENDER_STATS") != nullptr),
          census_enabled_(std::getenv("WETTER_RENDER_CENSUS") != nullptr),
          geo_log_requested_(std::getenv("WETTER_GEO_LOG") != nullptr) {
        std::fprintf(stderr, "[render] soft: F3DEX interpreter + software RDP\n");
        std::fflush(stderr);
    }

    ~SoftRenderer() override {
        if (stats_enabled_ || census_enabled_) report();
    }

    void run_task(uint32_t dl) override {
        const uint32_t dl_address = dl & AddressMask;

        if (dl_count_ == 0) {
            std::fprintf(stderr, "[render] first display list: 0x%08X -> RDRAM 0x%06X\n", dl, dl_address);
            std::fflush(stderr);
        }

        ++dl_count_;

        static const char* task_log = std::getenv("WETTER_TASK_LOG");
        if (task_log != nullptr && *task_log != '\0') {
            char list[1024];
            char entry[32];
            std::snprintf(list, sizeof(list), ",%s,", task_log);
            std::snprintf(entry, sizeof(entry), ",%llu,", static_cast<unsigned long long>(rdp_.frames()));
            if (std::strstr(list, entry) != nullptr) {
                front::GbiCensus census;
                front::gbi_census_display_list(rdram_, dl_address, census);
                std::fprintf(stderr,
                             "[task] frame %llu dl 0x%06X: cmds %llu "
                             "tri1 %u tri2 %u quad %u vtx %u mtx %u popmtx %u dl %u "
                             "branchz %u texturerect %u setimg %u truncated %d\n",
                             static_cast<unsigned long long>(rdp_.frames()), dl_address,
                             static_cast<unsigned long long>(census.commands), census.opcode_count[front::GBI_TRI1],
                             census.opcode_count[front::GBI_TRI2], census.opcode_count[front::GBI_QUAD],
                             census.opcode_count[front::GBI_VTX], census.opcode_count[front::GBI_MTX],
                             census.opcode_count[front::GBI_POPMTX], census.opcode_count[front::GBI_DL],
                             census.opcode_count[front::GBI_BRANCH_Z], census.opcode_count[front::GBI_TEXRECT],
                             census.opcode_count[front::GBI_SETTIMG], census.truncated ? 1 : 0);
                std::fflush(stderr);
            }
        }

        if (census_enabled_) {
            front::gbi_census_display_list(rdram_, dl_address, census_);
            if (dl_count_ == 1 || (dl_count_ % DisplayListsPerReport) == 0) {
                std::fprintf(stderr, "[gbi] after %u display lists:\n", dl_count_);
                census_.print(stderr);
            }
        }

        static const uint64_t pinned_log_frame = [] {
            const char* value = std::getenv("WETTER_GEO_FRAME");
            return value != nullptr ? static_cast<uint64_t>(std::strtoull(value, nullptr, 10)) : UINT64_MAX;
        }();
        front_.set_log_window(pinned_log_frame != UINT64_MAX ? rdp_.frames() >= pinned_log_frame : host_in_gameplay(),
                              geo_log_requested_);
        front_.begin_task();

        // WETTER_RENDER_TIME=1: average time spent drawing a display list.
        static const bool time_log = std::getenv("WETTER_RENDER_TIME") != nullptr;
        const auto t0 = std::chrono::steady_clock::now();
        front_.run(dl_address);
        rdp_.flush();
        if (time_log) {
            static double total_ms = 0.0;
            static uint32_t n = 0;
            total_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (++n == 120) {
                std::fprintf(stderr,
                             "[soft] %.2f ms per display list (last 120): fill %.2f texrect %.2f tri %.2f load %.2f\n",
                             total_ms / n, g_prof_ms[0] / n, g_prof_ms[1] / n, g_prof_ms[2] / n, g_prof_ms[3] / n);
                g_prof_ms[0] = g_prof_ms[1] = g_prof_ms[2] = g_prof_ms[3] = 0.0;
                uint64_t* k = g_prof_count;
                std::fprintf(stderr, "[soft]   per list: %llu tris, %llu walked, %llu covered, %llu written\n",
                             (unsigned long long)(k[0] / n), (unsigned long long)(k[1] / n),
                             (unsigned long long)(k[2] / n), (unsigned long long)(k[3] / n));
                k[0] = k[1] = k[2] = k[3] = 0;
                std::fprintf(stderr, "[soft]   flush %.2f ms, %.1f flushes per list\n", g_flush_ms / n,
                             double(g_flushes) / n);
                std::fprintf(stderr, "[soft]   workers: slowest %.2f ms, fastest %.2f ms\n", g_worker_ms[0] / n,
                             g_worker_ms[1] / n);
                g_worker_ms[0] = g_worker_ms[1] = 0.0;
                g_flush_ms = 0.0;
                g_flushes = 0;
                std::fflush(stderr);
                total_ms = 0.0;
                n = 0;
            }
        }

        if (stats_enabled_ && (dl_count_ % DisplayListsPerReport) == 0) {
            report();
        }
    }

    void compose(const ViRegs& vi) override {
        if (!vi_reported_) {
            vi_reported_ = true;
            std::fprintf(stderr, "[render] first VI swap: ORIGIN 0x%08X, WIDTH %u, STATUS 0x%08X\n", vi.origin,
                         vi.width, vi.status);
            std::fflush(stderr);
        }

        static const uint64_t capture_at = [] {
            const char* value = std::getenv("WETTER_CAPTURE_AT");
            return value != nullptr ? static_cast<uint64_t>(std::strtoull(value, nullptr, 10)) : 0ull;
        }();
        const bool by_frame = capture_at != 0u && rdp_.frames() + 2u == capture_at;
        if (capture_requested_.exchange(false) || by_frame) {
            const uint64_t draw_frame = by_frame ? capture_at - 1u : rdp_.frames() + 1u;
            rdp_.arm_capture(draw_frame, capture_path_.empty() ? std::string("capture.ppm") : capture_path_);
        }

        rdp_.compose_frame(vi.origin, vi.width, vi.status, vi.v_start, vi.y_scale);
        frame_stats_line();
    }

    Frame frame() const override {
        Frame f;
        f.pixels = reinterpret_cast<const uint32_t*>(rdp_.frame_pixels());
        f.width = rdp_.frame_width();
        f.height = rdp_.frame_height();
        f.stride = MaxFramebufferWidth;
        return f;
    }

    uint64_t frames() const override { return rdp_.frames(); }
    uint64_t triangles_submitted() const override { return front_.rsp().stats().triangles_submitted; }

    void request_capture(const std::string& path) override {
        capture_path_ = path;
        capture_requested_.store(true);
    }

    double bench(const uint8_t* ram, uint32_t dl, int n) override {
        // A private copy: the game keeps running on the live memory meanwhile.
        std::vector<uint8_t> image(RdramSize);
        uint8_t* live = rdram_;
        rdp_.set_rdram(image.data());
        front_.set_rdram(image.data());
        rdram_ = image.data();
        double total = 0.0;
        for (int i = 0; i < n; ++i) {
            std::memcpy(image.data(), ram, RdramSize);
            front_.begin_task();
            const auto t0 = std::chrono::steady_clock::now();
            front_.run(dl & AddressMask);
            rdp_.flush();
            total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }
        rdram_ = live;
        rdp_.set_rdram(live);
        front_.set_rdram(live);
        return n > 0 ? total / n : 0.0;
    }

    void* worker_handle(int i) const override { return rdp_.worker_handle(static_cast<size_t>(i)); }

    void report() override {
        const Rdp::Stats& s = rdp_.stats();
        std::fprintf(stderr,
                     "[render] %u display lists, %llu fillrects, %llu texrects, %llu triangles "
                     "drawn, %llu pixels, %llu texels, %llu frames\n",
                     dl_count_, static_cast<unsigned long long>(s.fill_rects),
                     static_cast<unsigned long long>(s.tex_rects), static_cast<unsigned long long>(s.triangles_drawn),
                     static_cast<unsigned long long>(s.pixels_drawn), static_cast<unsigned long long>(s.texels_sampled),
                     static_cast<unsigned long long>(s.frames));
        std::fprintf(stderr,
                     "[render] %llu triangles waiting on the vertex pipeline, "
                     "%llu unsupported texture format reads\n",
                     static_cast<unsigned long long>(front_.triangles_waiting()),
                     static_cast<unsigned long long>(s.unsupported_texture_format));

        const front::Rsp::Stats& g = front_.rsp().stats();
        std::fprintf(stderr,
                     "[rsp] %llu vertices transformed, %llu rejected, %llu triangles submitted, "
                     "%llu culled by winding, %llu clipped away, %llu degenerate, %llu clipped, %llu matrices "
                     "(%llu forced), %llu viewports, %llu pops on an empty stack, %llu pushes "
                     "the stack refused\n",
                     static_cast<unsigned long long>(g.vertices_transformed),
                     static_cast<unsigned long long>(g.vertices_rejected),
                     static_cast<unsigned long long>(g.triangles_submitted),
                     static_cast<unsigned long long>(g.triangles_culled),
                     static_cast<unsigned long long>(g.triangles_empty),
                     static_cast<unsigned long long>(g.triangles_degenerate),
                     static_cast<unsigned long long>(g.triangles_clipped), static_cast<unsigned long long>(g.matrices),
                     static_cast<unsigned long long>(g.force_matrices), static_cast<unsigned long long>(g.viewports),
                     static_cast<unsigned long long>(g.stack_underflows),
                     static_cast<unsigned long long>(g.stack_overflows));
        std::fflush(stderr);
    }

private:
    void frame_stats_line() {
        static const bool enabled = std::getenv("WETTER_FRAME_STATS") != nullptr;
        if (!enabled) return;

        const Rdp::Stats& s = rdp_.stats();
        const front::Rsp::Stats& g = front_.rsp().stats();

        struct Counters {
            uint64_t dl, tri_sub, tri_empty, tri_degen, tri_culled, tri_clipped, tri_drawn, tri_skipped, fill,
                texrect, pixels;
        };
        static Counters previous{};

        const Counters now{ dl_count_,         g.triangles_submitted, g.triangles_empty,  g.triangles_degenerate,
                            g.triangles_culled, g.triangles_clipped,   s.triangles_drawn,  s.triangles_skipped,
                            s.fill_rects,       s.tex_rects,           s.pixels_drawn };

        std::fprintf(stderr,
                     "[frame] %llu %s dl %llu sub %llu empty %llu degen %llu culled %llu "
                     "clipped %llu drawn %llu skipped %llu fill %llu rect %llu px %llu\n",
                     static_cast<unsigned long long>(rdp_.frames()), host_in_gameplay() ? "gameplay" : "menu",
                     static_cast<unsigned long long>(now.dl - previous.dl),
                     static_cast<unsigned long long>(now.tri_sub - previous.tri_sub),
                     static_cast<unsigned long long>(now.tri_empty - previous.tri_empty),
                     static_cast<unsigned long long>(now.tri_degen - previous.tri_degen),
                     static_cast<unsigned long long>(now.tri_culled - previous.tri_culled),
                     static_cast<unsigned long long>(now.tri_clipped - previous.tri_clipped),
                     static_cast<unsigned long long>(now.tri_drawn - previous.tri_drawn),
                     static_cast<unsigned long long>(now.tri_skipped - previous.tri_skipped),
                     static_cast<unsigned long long>(now.fill - previous.fill),
                     static_cast<unsigned long long>(now.texrect - previous.texrect),
                     static_cast<unsigned long long>(now.pixels - previous.pixels));
        std::fflush(stderr);
        previous = now;
    }

    uint8_t* rdram_ = nullptr;
    RdpFarm rdp_;
    front::Frontend front_;
    front::GbiCensus census_{};

    uint32_t dl_count_ = 0;
    bool stats_enabled_ = false;
    bool census_enabled_ = false;
    bool geo_log_requested_ = false;
    bool vi_reported_ = false;

    std::atomic<bool> capture_requested_{ false };
    std::string capture_path_;
};

}  // namespace

}  // namespace wetter::soft

namespace wetter::soft {

std::unique_ptr<Renderer> create_soft(uint8_t* rdram, const Options& options) {
    return std::make_unique<SoftRenderer>(rdram, options);
}

}  // namespace wetter::soft
