// Wetter: N64 display-list renderers.
//
// A Renderer interprets F3DEX display lists straight out of the game's memory
// (RDRAM in the N64Recomp word-swapped layout: byte A lives at rdram[A ^ 3]) and
// produces the picture the VI would show. Backends:
//   Soft  the RDP in software, drawing into RDRAM as the hardware does
//   Hard  the RDP on the GPU (OpenGL ES 3.0 or desktop GL 3.3 core), drawing
//         into host-side render targets; the host owns the GL context
//
// Wetter knows nothing about any particular game or runtime: the host hands it
// memory and tasks, asks it for frames, and may lend it a few hooks.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace wetter {

enum class Backend { Soft, Hard };

// Optional host callbacks. Traces and sampled logs use the gameplay ones to
// decide when to fire; frame_composed is told the count after every frame.
struct Hooks {
    bool (*in_gameplay)() = nullptr;
    uint64_t (*first_gameplay_frame)() = nullptr;
    uint64_t (*gameplay_frames)() = nullptr;
    void (*frame_composed)(uint64_t frame) = nullptr;
};

// How a renderer reads the display lists.
struct Options {
    // Take every G_BRANCH_Z instead of comparing the vertex's depth with zval.
    // Some games are drawn this way on purpose (they pick their level of
    // detail through it and look wrong with the real test).
    bool force_branch_z = false;

    // Hard: the host's GL loader (SDL_GL_GetProcAddress, eglGetProcAddress, ...),
    // and whether the context current on the calling thread is OpenGL ES 3.0
    // (otherwise desktop GL 3.3 core). Every call into a Hard renderer must be
    // made on that thread.
    void* (*gl_get_proc_address)(const char* name) = nullptr;
    bool gles = false;
    // Hard: render targets are this many times the game's framebuffer size.
    int scale = 1;
    // Hard: what it draws lives on the GPU. A texture load from an image it
    // drew always gets that image written back to RDRAM first; with
    // copy_back_every_task, every image drawn is also written back at the end
    // of each task, for games whose CPU reads its framebuffer (a readback a
    // frame).
    bool copy_back_every_task = false;
    // Hard: the display's aspect ratio. Wider than 4:3 widens the render targets
    // and, for a viewport as wide as the image, the 3D view around its centre;
    // 2D stays in the centred 4:3 area (a fill across the image spans it all).
    float aspect = 4.0f / 3.0f;
};

// The VI registers a frame is composed from.
struct ViRegs {
    uint32_t origin = 0;
    uint32_t width = 0;
    uint32_t status = 0;
    uint32_t v_start = 0;
    uint32_t y_scale = 0;
};

// A composed frame: ARGB8888, `stride` pixels per row.
struct Frame {
    const uint32_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
};

class Renderer {
public:
    static std::unique_ptr<Renderer> create(Backend backend, uint8_t* rdram, const Options& options = {},
                                            const Hooks& hooks = {});
    virtual ~Renderer() = default;

    // Runs one graphics task's display list (an RDRAM address; segment and KSEG
    // bits are masked off). Everything it draws is finished when this returns.
    virtual void run_task(uint32_t dl_address) = 0;

    // Composes the picture the VI shows from `vi`; frame() then returns it.
    virtual void compose(const ViRegs& vi) = 0;
    virtual Frame frame() const = 0;

    // GPU backends draw the composed picture into the context's default
    // framebuffer, `width` x `height`, at 4:3 with bars; the host then swaps.
    // Soft draws nothing here: the host presents frame() itself.
    virtual void present(int width, int height) {
        (void)width;
        (void)height;
    }

    // Frames composed so far, and triangles the display lists have submitted.
    virtual uint64_t frames() const = 0;
    virtual uint64_t triangles_submitted() const = 0;

    // Traces the next frame's draws and writes a capture named from `path`.
    virtual void request_capture(const std::string& path) = 0;

    // Runs `dl_address` `runs` times, each from a fresh copy of `ram` (a whole
    // RDRAM image), and returns the average milliseconds. Live memory is not
    // touched, so the game may keep running.
    virtual double bench(const uint8_t* ram, uint32_t dl_address, int runs) = 0;

    // The native handle of worker thread i, for a sampling profiler; or null.
    virtual void* worker_handle(int i) const = 0;

    // Prints running totals to stderr.
    virtual void report() = 0;
};

}  // namespace wetter
