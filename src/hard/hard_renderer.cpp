// hard: the Renderer. The shared front end walks the display lists; HardRdp
// draws them on the GPU.

#include <wetter/wetter.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "front/frontend.h"
#include "gl.h"
#include "hard_rdp.h"

namespace wetter::hard {

namespace {

constexpr uint32_t AddressMask = 0x3FFFFFFu;

class HardRenderer final : public Renderer {
public:
    HardRenderer(uint8_t* rdram, const Options& options)
        : rdram_(rdram),
          rdp_(rdram, api_, options.gles, options.scale, options.copy_back_every_task, extension(options)),
          front_(rdram, rdp_, options.force_branch_z, extension(options)) {}

    static float extension(const Options& options) {
        const float e = options.aspect / (4.0f / 3.0f);
        return e > 1.0f ? e : 1.0f;
    }

    bool init(const Options& options) {
        if (!gl::load(api_, options.gl_get_proc_address)) return false;
        if (!rdp_.init()) return false;
        std::fprintf(stderr, "[render] hard: F3DEX interpreter + GPU RDP (%s, scale %d, aspect %.3f)\n",
                     options.gles ? "GLES 3.0" : "GL 3.3 core", options.scale, options.aspect);
        std::fflush(stderr);
        return true;
    }

    void run_task(uint32_t dl) override {
        static const bool time_log = std::getenv("WETTER_RENDER_TIME") != nullptr;
        const auto t0 = std::chrono::steady_clock::now();
        front_.begin_task();
        front_.run(dl & AddressMask);
        rdp_.end_task();
        if (time_log) {
            static double total_ms = 0.0;
            static uint32_t n = 0;
            total_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (++n == 120) {
                // Per display list: all of it, the GL draws within it, new
                // textures within it; the rest is the front end and batching.
                static HardRdp::Stats last{};
                const HardRdp::Stats& s = rdp_.stats();
                std::fprintf(stderr,
                             "[hard] %.2f ms per display list (CPU; last 120): draws %.2f ms (%.0f of them), new "
                             "textures %.2f ms (%.1f); %llu write-backs so far\n",
                             total_ms / n, (s.draw_ms - last.draw_ms) / n, double(s.draws - last.draws) / n,
                             (s.texture_ms - last.texture_ms) / n, double(s.textures_decoded - last.textures_decoded) / n,
                             static_cast<unsigned long long>(s.write_backs));
                std::fflush(stderr);
                last = s;
                total_ms = 0.0;
                n = 0;
            }
        }
    }

    void compose(const ViRegs& vi) override { rdp_.compose(vi.origin, vi.width, vi.status, vi.v_start, vi.y_scale); }

    void present(int width, int height) override { rdp_.present(width, height); }

    Frame frame() const override {
        // Read back on demand (debug captures); the GL thread only.
        auto* self = const_cast<HardRenderer*>(this);
        Frame f;
        if (!self->rdp_.read_shown(self->readback_, f.width, f.height)) return f;
        f.pixels = readback_.data();
        f.stride = f.width;
        return f;
    }

    uint64_t frames() const override { return rdp_.frames(); }
    uint64_t triangles_submitted() const override { return front_.rsp().stats().triangles_submitted; }

    void request_capture(const std::string& path) override {
        std::fprintf(stderr, "[hard] capture to %s: not implemented yet\n", path.c_str());
    }

    double bench(const uint8_t* ram, uint32_t dl, int n) override {
        std::vector<uint8_t> image(front::RdramSize);
        uint8_t* live = rdram_;
        rdp_.set_rdram(image.data());
        front_.set_rdram(image.data());
        double total = 0.0;
        for (int i = 0; i < n; ++i) {
            std::memcpy(image.data(), ram, front::RdramSize);
            const auto t0 = std::chrono::steady_clock::now();
            front_.begin_task();
            front_.run(dl & AddressMask);
            rdp_.flush();
            api_.Finish();   // count the GPU's work too
            total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }
        rdp_.set_rdram(live);
        front_.set_rdram(live);
        return n > 0 ? total / n : 0.0;
    }

    void* worker_handle(int) const override { return nullptr; }

    void report() override {
        const HardRdp::Stats& s = rdp_.stats();
        std::fprintf(stderr, "[hard] %llu triangles, %llu fills, %llu texrects, %llu draws, %llu targets\n",
                     static_cast<unsigned long long>(s.triangles), static_cast<unsigned long long>(s.fill_rects),
                     static_cast<unsigned long long>(s.tex_rects), static_cast<unsigned long long>(s.draws),
                     static_cast<unsigned long long>(s.targets_created));
        std::fprintf(stderr, "[hard] %llu textures decoded, %llu programs, %llu write-backs\n",
                     static_cast<unsigned long long>(s.textures_decoded), static_cast<unsigned long long>(s.programs),
                     static_cast<unsigned long long>(s.write_backs));
        std::fflush(stderr);
    }

private:
    uint8_t* rdram_ = nullptr;
    gl::Api api_;
    HardRdp rdp_;
    front::Frontend front_;
    std::vector<uint32_t> readback_;
};

}  // namespace

std::unique_ptr<Renderer> create_hard(uint8_t* rdram, const Options& options) {
    auto r = std::make_unique<HardRenderer>(rdram, options);
    if (!r->init(options)) return nullptr;
    return r;
}

}  // namespace wetter::hard
