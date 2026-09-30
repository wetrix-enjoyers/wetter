// front end: the colour combiner's combine word, decoded. G_SETCOMBINE packs
// two cycles of (A - B) * C + D for colour and for alpha; these tables turn each
// field into the operand it names. Backends evaluate the operands their own way.
#pragma once

#include <cstdint>

namespace wetter::front::cc {

// Operand sources, one table row per source, per channel (r, g, b, a).
enum Source : uint8_t {
    S_COMBINED, S_TEXEL0, S_TEXEL1, S_PRIM, S_SHADE, S_ENV, S_ONE, S_ZERO,
    S_COMBINED_A, S_TEXEL0_A, S_TEXEL1_A, S_PRIM_A, S_SHADE_A, S_ENV_A,
    S_LOD_FRAC, S_PRIM_LOD_FRAC, S_COUNT
};

// The mux tables: field value -> source.
constexpr Source kRgbA[16] = { S_COMBINED, S_TEXEL0, S_TEXEL1, S_PRIM, S_SHADE, S_ENV, S_ONE, S_ZERO,
                               S_ZERO, S_ZERO, S_ZERO, S_ZERO, S_ZERO, S_ZERO, S_ZERO, S_ZERO };
constexpr Source kRgbB[16] = { S_COMBINED, S_TEXEL0, S_TEXEL1, S_PRIM, S_SHADE, S_ENV, S_ZERO, S_ZERO,
                               S_ZERO, S_ZERO, S_ZERO, S_ZERO, S_ZERO, S_ZERO, S_ZERO, S_ZERO };
constexpr Source kRgbC[32] = { S_COMBINED, S_TEXEL0, S_TEXEL1, S_PRIM, S_SHADE, S_ENV, S_ZERO, S_COMBINED_A,
                               S_TEXEL0_A, S_TEXEL1_A, S_PRIM_A, S_SHADE_A, S_ENV_A, S_LOD_FRAC,
                               S_PRIM_LOD_FRAC, S_ZERO,
                               S_ZERO, S_ZERO, S_ZERO, S_ZERO, S_ZERO, S_ZERO, S_ZERO, S_ZERO,
                               S_ZERO, S_ZERO, S_ZERO, S_ZERO, S_ZERO, S_ZERO, S_ZERO, S_ZERO };
constexpr Source kRgbD[8] = { S_COMBINED, S_TEXEL0, S_TEXEL1, S_PRIM, S_SHADE, S_ENV, S_ONE, S_ZERO };
constexpr Source kAlphaABD[8] = { S_COMBINED, S_TEXEL0, S_TEXEL1, S_PRIM, S_SHADE, S_ENV, S_ONE, S_ZERO };
constexpr Source kAlphaC[8] = { S_LOD_FRAC, S_TEXEL0, S_TEXEL1, S_PRIM, S_SHADE, S_ENV, S_PRIM_LOD_FRAC, S_ZERO };

struct CycleFields {
    uint8_t rgb_a, rgb_b, rgb_c, rgb_d, alpha_a, alpha_b, alpha_c, alpha_d;
};

inline CycleFields cycle_fields(uint32_t w0, uint32_t w1, int cycle) {
    CycleFields f;
    if (cycle == 0) {
        f.rgb_a = (w0 >> 20) & 0xF;  f.rgb_c = (w0 >> 15) & 0x1F;
        f.alpha_a = (w0 >> 12) & 0x7; f.alpha_c = (w0 >> 9) & 0x7;
        f.rgb_b = (w1 >> 28) & 0xF;  f.rgb_d = (w1 >> 15) & 0x7;
        f.alpha_b = (w1 >> 12) & 0x7; f.alpha_d = (w1 >> 9) & 0x7;
    } else {
        f.rgb_a = (w0 >> 5) & 0xF;   f.rgb_c = w0 & 0x1F;
        f.alpha_a = (w1 >> 21) & 0x7; f.alpha_c = (w1 >> 18) & 0x7;
        f.rgb_b = (w1 >> 24) & 0xF;  f.rgb_d = (w1 >> 6) & 0x7;
        f.alpha_b = (w1 >> 3) & 0x7;  f.alpha_d = w1 & 0x7;
    }
    return f;
}

inline uint8_t swap_texels(uint8_t s) {
    switch (s) {
        case S_TEXEL0: return S_TEXEL1;
        case S_TEXEL1: return S_TEXEL0;
        case S_TEXEL0_A: return S_TEXEL1_A;
        case S_TEXEL1_A: return S_TEXEL0_A;
        default: return s;
    }
}


// The operand sources of the combine word (low 24 bits of w0, and w1), as the
// two slots a backend evaluates: slot 0 is a two-cycle combine's first cycle,
// slot 1 the last (or only) one. In one-cycle mode the RDP runs cycle 1's
// fields; in two-cycle mode cycle 1 sees the next tile as TEXEL0 and the first
// cycle's result as COMBINED. Each slot: rgb a b c d, alpha a b c d.
struct Slots {
    uint8_t sel[2][8];
};
inline Slots decode(uint32_t w0, uint32_t w1, bool two_cycle) {
    uint8_t raw[2][8];
    for (int cyc = 0; cyc < 2; ++cyc) {
        const CycleFields f = cycle_fields(w0, w1, cyc);
        uint8_t* s = raw[cyc];
        s[0] = kRgbA[f.rgb_a]; s[1] = kRgbB[f.rgb_b]; s[2] = kRgbC[f.rgb_c]; s[3] = kRgbD[f.rgb_d];
        s[4] = kAlphaABD[f.alpha_a]; s[5] = kAlphaABD[f.alpha_b]; s[6] = kAlphaC[f.alpha_c];
        s[7] = kAlphaABD[f.alpha_d];
    }
    Slots out;
    for (int i = 0; i < 8; ++i) {
        out.sel[0][i] = raw[0][i];
        out.sel[1][i] = two_cycle ? swap_texels(raw[1][i]) : raw[1][i];
    }
    return out;
}

}  // namespace wetter::front::cc
