// front end: the RDP's texture memory -- the texture image, the eight tile
// descriptors, the 4 KB TMEM and the loads that fill it, and the decode of one
// texel. Every backend samples through this, so they all see the same texels.
#pragma once

#include <cstdint>

namespace wetter::front {

struct TileDesc {
    bool used = false;
    uint32_t fmt = 0;
    uint32_t siz = 0;
    uint32_t palette = 0;
    uint32_t line = 0;
    uint32_t tmem = 0;
    uint32_t cm_t = 0, mask_t = 0, shift_t = 0;
    uint32_t cm_s = 0, mask_s = 0, shift_s = 0;
    uint32_t uls = 0, ult = 0, lrs = 0, lrt = 0;   // 10.2
};

// A G_SETTIMG / G_SETCIMG / G_SETZIMG image.
struct ImageDesc {
    uint32_t fmt = 0;
    uint32_t siz = 0;
    uint32_t width = 0;
    uint32_t address = 0;
    bool valid = false;
};

// The texture-lookup mode (other mode H bits 14-15): 0 none, 2 RGBA16, 3 IA16.
inline uint32_t tlut_mode(uint32_t other_mode_h) { return (other_mode_h >> 14) & 3u; }

class Tmem {
public:
    void set_rdram(const uint8_t* rdram) { rdram_ = rdram; }

    void set_texture_image(uint32_t fmt, uint32_t siz, uint32_t width, uint32_t address);
    void set_tile(uint32_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint32_t tile, uint32_t palette,
                  uint32_t cm_t, uint32_t mask_t, uint32_t shift_t, uint32_t cm_s, uint32_t mask_s, uint32_t shift_s);
    void set_tile_size(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt);
    void load_block(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt);
    void load_tile(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt);
    void load_tlut(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt);

    // One texel of `tile` at texel (s, t) -- already clamped or wrapped --
    // as RGBA8 packed with red in the low byte.
    uint32_t texel(const TileDesc& tile, uint32_t s, uint32_t t, uint32_t tlut) const;

    // A fingerprint of everything texel() would read for a w x h region of
    // `tile` under `tlut`: the descriptor, the TMEM rows, the palette.
    uint64_t fingerprint(const TileDesc& tile, uint32_t w, uint32_t h, uint32_t tlut) const;

    static uint32_t width(const TileDesc& tile) { return ((tile.lrs - tile.uls) >> 2) + 1u; }
    static uint32_t height(const TileDesc& tile) { return ((tile.lrt - tile.ult) >> 2) + 1u; }

    // Public so a backend can keep its own names for them.
    TileDesc tiles[8];
    uint8_t bytes[4096] = {};
    ImageDesc image;
    uint32_t palette_address = 0;
    uint32_t palette_count = 0;

private:
    uint8_t read_byte(uint32_t address) const;
    const uint8_t* rdram_ = nullptr;
};

}  // namespace wetter::front
