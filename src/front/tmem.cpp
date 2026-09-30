// front end: texture memory. See tmem.h.

#include "tmem.h"

#include <algorithm>
#include <cstring>

#include "rdp_sink.h"

namespace wetter::front {

namespace {

inline uint32_t expand5(uint32_t v) {
    const uint32_t five = v & 0x1Fu;
    return (five << 3) | (five >> 2);
}
inline uint32_t expand4(uint32_t v) { return (v & 0x0Fu) * 17u; }
inline uint32_t expand3(uint32_t v) { return ((v & 0x07u) * 255u + 3u) / 7u; }
inline uint32_t pack(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | (g << 8) | (b << 16) | (a << 24); }

inline uint32_t texel_bytes(uint32_t texels, uint32_t siz) { return (texels << siz) >> 1; }

}  // namespace

uint8_t Tmem::read_byte(uint32_t address) const {
    if (address >= RdramSize) return 0;
    return rdram_[address ^ 3u];
}

void Tmem::set_texture_image(uint32_t fmt, uint32_t siz, uint32_t width, uint32_t address) {
    image.fmt = fmt;
    image.siz = siz;
    image.width = width;
    image.address = address & 0x3FFFFFFu;
    image.valid = true;
}

void Tmem::set_tile(uint32_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint32_t tile, uint32_t palette,
                    uint32_t cm_t, uint32_t mask_t, uint32_t shift_t, uint32_t cm_s, uint32_t mask_s,
                    uint32_t shift_s) {
    if (tile >= 8) return;
    TileDesc& t = tiles[tile];
    t.used = true;
    t.fmt = fmt;
    t.siz = siz;
    t.palette = palette;
    t.cm_t = cm_t;
    t.mask_t = mask_t;
    t.shift_t = shift_t;
    t.cm_s = cm_s;
    t.mask_s = mask_s;
    t.shift_s = shift_s;
    t.line = line;
    t.tmem = tmem;
}

void Tmem::set_tile_size(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    if (tile >= 8) return;
    TileDesc& t = tiles[tile];
    t.used = true;
    t.uls = uls;
    t.ult = ult;
    t.lrs = lrs;
    t.lrt = lrt;
}

void Tmem::load_block(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) {
    if (tile >= 8 || !image.valid) return;
    const TileDesc& t = tiles[tile];
    const uint32_t siz = image.siz;
    const uint32_t texels = lrs - uls + 1u;
    const uint32_t count = std::max<uint32_t>(texel_bytes(texels, siz), 1u);
    const uint32_t qwords = std::min<uint32_t>((count + 7u) / 8u, 512u);
    const uint32_t src = image.address + texel_bytes(ult * image.width + uls, siz);
    const uint32_t dst = t.tmem * 8u;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < qwords; ++i) {
        // dxt counts lines: odd lines are stored with their words swapped.
        const bool odd = ((acc >> 11) & 1u) != 0u;
        acc += dxt;
        uint8_t q[8];
        const uint32_t at = src + i * 8u;
        if ((at & 3u) == 0u && at + 8u <= RdramSize) {
            // Two big-endian words, stored as native words.
            uint32_t w[2];
            std::memcpy(w, rdram_ + at, 8);
            for (uint32_t k = 0; k < 2; ++k) {
                q[k * 4 + 0] = static_cast<uint8_t>(w[k] >> 24);
                q[k * 4 + 1] = static_cast<uint8_t>(w[k] >> 16);
                q[k * 4 + 2] = static_cast<uint8_t>(w[k] >> 8);
                q[k * 4 + 3] = static_cast<uint8_t>(w[k]);
            }
        } else {
            for (uint32_t b = 0; b < 8; ++b) q[b] = read_byte(at + b);
        }
        if (siz == 3) {
            // RGBA32 splits each texel: red/green low half, blue/alpha high half.
            const uint32_t bank = (dst + i * 4u) ^ (odd ? 4u : 0u);
            bytes[(bank + 0) & 0x7FF] = q[0];
            bytes[(bank + 1) & 0x7FF] = q[1];
            bytes[(bank + 2) & 0x7FF] = q[4];
            bytes[(bank + 3) & 0x7FF] = q[5];
            bytes[((bank + 0) & 0x7FF) | 0x800] = q[2];
            bytes[((bank + 1) & 0x7FF) | 0x800] = q[3];
            bytes[((bank + 2) & 0x7FF) | 0x800] = q[6];
            bytes[((bank + 3) & 0x7FF) | 0x800] = q[7];
        } else {
            const uint32_t base = dst + i * 8u;
            for (uint32_t b = 0; b < 8; ++b) bytes[(base + (b ^ (odd ? 4u : 0u))) & 0xFFF] = q[b];
        }
    }
}

void Tmem::load_tile(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    if (tile >= 8 || !image.valid) return;
    TileDesc& t = tiles[tile];
    const uint32_t siz = image.siz;
    const uint32_t x0 = uls >> 2, y0 = ult >> 2, x1 = lrs >> 2, y1 = lrt >> 2;
    const uint32_t line = t.line * 8u;
    const uint32_t dst = t.tmem * 8u;
    for (uint32_t y = 0; y + y0 <= y1 && y < 1024; ++y) {
        const uint32_t row = image.address + texel_bytes((y0 + y) * image.width + x0, siz);
        const uint32_t x = (y & 1u) ? 4u : 0u;
        if (siz == 3) {
            for (uint32_t i = 0; i + x0 <= x1; ++i) {
                const uint32_t bank = (dst + y * line + i * 2u) ^ x;
                bytes[bank & 0x7FF] = read_byte(row + i * 4u + 0);
                bytes[(bank + 1) & 0x7FF] = read_byte(row + i * 4u + 1);
                bytes[(bank & 0x7FF) | 0x800] = read_byte(row + i * 4u + 2);
                bytes[((bank + 1) & 0x7FF) | 0x800] = read_byte(row + i * 4u + 3);
            }
        } else {
            const uint32_t n = (texel_bytes(x1 - x0 + 1u, siz) + 7u) & ~7u;
            for (uint32_t b = 0; b < n; ++b) bytes[((dst + y * line + b) ^ x) & 0xFFF] = read_byte(row + b);
        }
    }
    t.used = true;
    t.uls = uls;
    t.ult = ult;
    t.lrs = lrs;
    t.lrt = lrt;
}

void Tmem::load_tlut(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    (void)lrt;
    if (tile >= 8 || !image.valid) return;
    const TileDesc& t = tiles[tile];
    const uint32_t count = (lrs >> 2) - (uls >> 2) + 1u;
    const uint32_t src = image.address + ((ult >> 2) * image.width + (uls >> 2)) * 2u;
    const uint32_t dst = t.tmem * 8u;
    // Each entry is stored four times over, one per TMEM bank.
    for (uint32_t i = 0; i < count && i < 256u; ++i) {
        const uint8_t hi = read_byte(src + i * 2u), lo = read_byte(src + i * 2u + 1u);
        for (uint32_t k = 0; k < 4; ++k) {
            bytes[(dst + i * 8u + k * 2u) & 0xFFF] = hi;
            bytes[(dst + i * 8u + k * 2u + 1u) & 0xFFF] = lo;
        }
    }
    palette_address = src;
    palette_count = count;
}

uint32_t Tmem::texel(const TileDesc& tile, uint32_t s, uint32_t t, uint32_t tlut) const {
    const uint32_t row = tile.tmem * 8u + t * tile.line * 8u;
    const uint32_t xr = (t & 1u) ? 4u : 0u;   // odd rows have their words swapped
    const bool use_tlut = tlut >= 2u && tile.siz <= 1u;

    auto palette = [&](uint32_t index) {
        const uint32_t a = 0x800u + (index & 0xFFu) * 8u;
        const uint32_t v = (uint32_t(bytes[a]) << 8) | bytes[a + 1];
        if (tlut == 3u) return pack(v >> 8, v >> 8, v >> 8, v & 0xFFu);
        return pack(expand5(v >> 11), expand5(v >> 6), expand5(v >> 1), (v & 1u) ? 255u : 0u);
    };

    switch (tile.siz) {
        case 0: {   // 4-bit
            const uint8_t packed = bytes[((row + (s >> 1)) ^ xr) & 0xFFF];
            const uint32_t n = (s & 1u) ? (packed & 0x0Fu) : (packed >> 4);
            if (use_tlut) return palette((tile.palette << 4) | n);
            if (tile.fmt == 3) {   // IA4
                const uint32_t i = expand3(n >> 1);
                return pack(i, i, i, (n & 1u) ? 255u : 0u);
            }
            const uint32_t i = expand4(n);   // I4 (and CI without a TLUT)
            return pack(i, i, i, i);
        }
        case 1: {   // 8-bit
            const uint32_t v = bytes[((row + s) ^ xr) & 0xFFF];
            if (use_tlut) return palette(v);
            if (tile.fmt == 3) {   // IA8
                const uint32_t i = expand4(v >> 4);
                return pack(i, i, i, expand4(v));
            }
            return pack(v, v, v, v);   // I8
        }
        case 2: {   // 16-bit
            const uint32_t a = (row + s * 2u) ^ xr;
            const uint32_t v = (uint32_t(bytes[a & 0xFFF]) << 8) | bytes[(a + 1) & 0xFFF];
            if (tile.fmt == 3) return pack(v >> 8, v >> 8, v >> 8, v & 0xFFu);   // IA16
            return pack(expand5(v >> 11), expand5(v >> 6), expand5(v >> 1), (v & 1u) ? 255u : 0u);
        }
        case 3: {   // RGBA32, split banks
            const uint32_t a = ((row + s * 2u) ^ xr) & 0x7FF;
            return pack(bytes[a], bytes[(a + 1) & 0x7FF], bytes[a | 0x800], bytes[((a + 1) & 0x7FF) | 0x800]);
        }
    }
    return 0xFFFFFFFFu;
}

uint64_t Tmem::fingerprint(const TileDesc& tile, uint32_t w, uint32_t h, uint32_t tlut) const {
    // FNV-1a over the descriptor and the bytes the region reads.
    uint64_t hash = 0xCBF29CE484222325ull;
    auto mix = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            hash ^= (v >> (i * 8)) & 0xFFu;
            hash *= 0x100000001B3ull;
        }
    };
    mix(tile.fmt | (tile.siz << 4) | (tile.palette << 8) | (uint64_t(tile.line) << 16) | (uint64_t(tile.tmem) << 32));
    mix(w | (uint64_t(h) << 16) | (uint64_t(tlut) << 32));
    const uint32_t row_bytes = tile.siz == 3 ? w * 2u : std::max<uint32_t>(texel_bytes(w, tile.siz), 1u);
    const uint32_t start = tile.tmem * 8u;
    const uint32_t span = std::min<uint32_t>((h == 0 ? 1u : h - 1u) * tile.line * 8u + row_bytes + 8u, 4096u);
    for (uint32_t i = 0; i < span; ++i) {
        const uint32_t a = (start + i) & 0xFFFu;
        hash ^= bytes[a];
        hash *= 0x100000001B3ull;
        if (tile.siz == 3) {
            hash ^= bytes[(a & 0x7FFu) | 0x800u];
            hash *= 0x100000001B3ull;
        }
    }
    if (tlut >= 2u && tile.siz <= 1u) {
        const uint32_t first = tile.siz == 0 ? 0x800u + tile.palette * 16u * 8u : 0x800u;
        const uint32_t n = tile.siz == 0 ? 16u * 8u : 256u * 8u;
        for (uint32_t i = 0; i < n; i += 8) {
            hash ^= bytes[(first + i) & 0xFFF];
            hash *= 0x100000001B3ull;
            hash ^= bytes[(first + i + 1) & 0xFFF];
            hash *= 0x100000001B3ull;
        }
    }
    return hash;
}

}  // namespace wetter::front
