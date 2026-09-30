# Wetter

N64 display-list renderers for recompiled games. One library, two modes:

- **soft**: the RDP in software. It draws into the game's RDRAM framebuffer the
  way the hardware does, on several threads.
- **hard**: the RDP on the GPU (OpenGL ES 3.0 or desktop GL 3.3 core), in the
  host's GL context, with an internal resolution scale and widescreen.

Both share one front end (`src/front`: the display-list walker, the RSP, the
GBI tables), which hands RDP commands to a backend through `front::RdpSink`.
Display lists are read straight out of game memory, in the N64Recomp layout
(words stored native, so byte `A` is at `rdram[A ^ 3]`). Wetter knows nothing
about any particular game or runtime.

## Use

```cmake
add_subdirectory(path/to/wetter ${CMAKE_BINARY_DIR}/wetter)
target_link_libraries(your_target PRIVATE wetter)
```

```cpp
#include <wetter/wetter.h>

wetter::Options options;                 // e.g. options.force_branch_z
auto renderer = wetter::Renderer::create(wetter::Backend::Soft, rdram, options);
renderer->run_task(task_data_ptr);       // for each graphics task
renderer->compose(vi_regs);              // for each VI swap
wetter::Frame f = renderer->frame();     // ARGB8888, f.stride pixels per row
```

For `Backend::Hard`, set `options.gl_get_proc_address`, `options.gles`, and
optionally `options.scale`, `options.aspect` (widescreen) and
`options.copy_back_every_task`; call every method on the thread whose GL context
is current, and `present(w, h)` after `compose` to draw into its default
framebuffer.

`wetter::Options::force_branch_z` takes every `G_BRANCH_Z` instead of testing
the vertex's depth, for games drawn that way. `wetter::Hooks` lets the host lend
a few callbacks: whether the game is in gameplay (traces fire only then), and a
per-frame notification.

## Build options

| Option | Default | |
|---|---|---|
| `WETTER_SOFT` | ON | the software renderer |
| `WETTER_HARD` | ON | the GPU renderer (no GL library is linked: the host lends `gl_get_proc_address`) |
| `WETTER_DEBUGINFO` | OFF | line tables, for profiling |

On x86 the software renderer's `rdp.cpp` builds with `-msse4.1`.

## Environment

| Variable | |
|---|---|
| `WETTER_SOFT_THREADS=n` | rasterizer workers (default: cores − 1, at most 7) |
| `WETTER_SOFT_BAND=n` | rows per band dealt to each worker (default 16) |
| `WETTER_RENDER_TIME=1` | milliseconds per display list, every 120 lists |
| `WETTER_WORKER_TIME=1` | per-worker time per flush |
| `WETTER_RENDER_STATS=1` | running totals |
| `WETTER_FRAME_STATS=1` | one counter line per frame |
| `WETTER_CAPTURE_AT=n` | trace and capture frame n |
| `WETTER_BLEND_LOG=1` | each distinct blender setup, once |

Traces, captures and the source map (`WETTER_TRI_TRACE`, `WETTER_TEXEL_TRACE`,
`WETTER_SOURCE_MAP`, `WETTER_TRACE_FRAME`, `WETTER_CAPTURE_AT`, and the fill and
rect logs) run the rasterizer on one thread.

## Profiling

`Renderer::bench()` replays one display list from a saved memory image, on a
private copy. `worker_handle()` exposes the worker threads so a host can sample
them. Build with `WETTER_DEBUGINFO=ON`, then summarise the sampled addresses:

```
python tools/soft_prof.py <executable> <file.samples>
```

## Thanks

Wetter owes a lot to two projects, for inspiration rather than code:

- **Fast3D** (from the Super Mario 64 PC port, and the forks after it, such as
  Perfect Dark's): hard follows its shape. It walks the display list, batches
  triangles, generates a shader per combiner, and keeps a render target per
  colour image.
- **RT64**: the reference we checked our pictures against while building soft
  and hard, and an education in how the RDP behaves.

## Licence

MIT; see `LICENSE`.
