# TODO

## Dreams (features the fuller Fast3D forks have)

Checked from memory against Perfect Dark's Fast3D fork and libultraship; confirm
the details before building any of them.

- **More GPU backends.** D3D11 and D3D12 on Windows, Metal on macOS, maybe Vulkan.
  `hard` is GL/GLES only. The backend-specific parts are `src/hard/gl.*` and the
  GL calls in `hard_rdp.cpp`.
- **3-point texture filtering.** The N64's own filter (three texels, not
  four), as an option beside bilinear and point.
- **MSAA / anti-aliasing** on hard's render targets.
- **Frame interpolation.** Draw in-between frames above the game's rate for
  smoother motion, without changing game speed.
- **Texture packs.** Replace a decoded texture by its fingerprint
  (`Tmem::fingerprint` already gives a stable key) with a high-resolution image
  from disk.

## Known gaps in hard

- **The first blender cycle reading memory** (two-cycle blends whose first cycle
  uses the framebuffer) is not modelled;
- **No mipmapping:** the combiner's LOD fractions read as 0.
- **Copy-mode texture rectangles** step like 1-cycle ones (as soft does), not with
  the hardware's quarter step.
- **No depth copy-back** to RDRAM; colour only.
- **Prim depth** (`G_ZS_PRIM`) is ignored.

## Performance head-room

- **Fewer draws:** carry the constant colours (prim, env, fog, blend) as vertex
  data, so primitives that differ only by colour share a draw.
- **A cheaper front end:** the RSP path is ~7 ms per list on the device.
