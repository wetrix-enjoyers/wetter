// F3DEX (GBI1) command numbers and a display-list census.

#ifndef WETTER_FRONT_GBI_H
#define WETTER_FRONT_GBI_H

#include <cstdint>
#include <cstdio>

namespace wetter::front {

enum GbiOpcode : uint8_t {
    GBI_SPNOOP            = 0x00,
    GBI_MTX               = 0x01,
    GBI_MOVEMEM           = 0x03,
    GBI_VTX               = 0x04,
    GBI_DL                = 0x06,
    GBI_SPRITE2D_BASE     = 0x09,

    GBI_LOAD_UCODE        = 0xAF,
    GBI_BRANCH_Z          = 0xB0,
    GBI_TRI2              = 0xB1,
    GBI_MODIFYVTX         = 0xB2,
    GBI_RDPHALF_2         = 0xB3,
    GBI_RDPHALF_1         = 0xB4,
    GBI_QUAD              = 0xB5,
    GBI_CLEARGEOMETRYMODE = 0xB6,
    GBI_SETGEOMETRYMODE   = 0xB7,
    GBI_ENDDL             = 0xB8,
    GBI_SETOTHERMODE_L    = 0xB9,
    GBI_SETOTHERMODE_H    = 0xBA,
    GBI_TEXTURE           = 0xBB,
    GBI_MOVEWORD          = 0xBC,
    GBI_POPMTX            = 0xBD,
    GBI_CULLDL            = 0xBE,
    GBI_TRI1              = 0xBF,

    GBI_RDPNOOP           = 0xC0,
    GBI_TEXRECT           = 0xE4,
    GBI_TEXRECTFLIP       = 0xE5,
    GBI_RDPLOADSYNC       = 0xE6,
    GBI_RDPPIPESYNC       = 0xE7,
    GBI_RDPTILESYNC       = 0xE8,
    GBI_RDPFULLSYNC       = 0xE9,
    GBI_SETKEYGB          = 0xEA,
    GBI_SETKEYR           = 0xEB,
    GBI_SETCONVERT        = 0xEC,
    GBI_SETSCISSOR        = 0xED,
    GBI_SETPRIMDEPTH      = 0xEE,
    GBI_RDPSETOTHERMODE   = 0xEF,
    GBI_LOADTLUT          = 0xF0,
    GBI_SETTILESIZE       = 0xF2,
    GBI_LOADBLOCK         = 0xF3,
    GBI_LOADTILE          = 0xF4,
    GBI_SETTILE           = 0xF5,
    GBI_FILLRECT          = 0xF6,
    GBI_SETFILLCOLOR      = 0xF7,
    GBI_SETFOGCOLOR       = 0xF8,
    GBI_SETBLENDCOLOR     = 0xF9,
    GBI_SETPRIMCOLOR      = 0xFA,
    GBI_SETENVCOLOR       = 0xFB,
    GBI_SETCOMBINE        = 0xFC,
    GBI_SETTIMG           = 0xFD,
    GBI_SETZIMG           = 0xFE,
    GBI_SETCIMG           = 0xFF,
};

// G_MOVEMEM viewport/matrix indices (the F3D names; F3DEX uses the same block).
enum GbiMoveMemIndex : uint8_t {
    GBI_MV_VIEWPORT     = 0x80,
    GBI_MV_LOOKATY      = 0x82,
    GBI_MV_LOOKATX      = 0x84,
    GBI_MV_L0           = 0x86,
    GBI_MV_MATRIX_1     = 0x9E,
    GBI_MV_MATRIX_2     = 0x98,
    GBI_MV_MATRIX_3     = 0x9A,
    GBI_MV_MATRIX_4     = 0x9C,
};

enum GbiMoveWordIndex : uint8_t {
    GBI_MW_MATRIX     = 0x00,
    GBI_MW_NUMLIGHT   = 0x02,
    GBI_MW_CLIP       = 0x04,
    GBI_MW_SEGMENT    = 0x06,
    GBI_MW_FOG        = 0x08,
    GBI_MW_LIGHTCOL   = 0x0A,
    GBI_MW_PERSPNORM  = 0x0E,
};

enum GbiMatrixFlag : uint8_t {
    GBI_MTX_PROJECTION = 0x01,
    GBI_MTX_LOAD       = 0x02,
    GBI_MTX_PUSH       = 0x04,
};

enum GbiGeometryMode : uint32_t {
    GBI_GEOM_ZBUFFER            = 0x00000001,
    GBI_GEOM_SHADE              = 0x00000004,
    GBI_GEOM_CULL_FRONT_F3D     = 0x00000200,
    GBI_GEOM_CULL_BACK_F3D      = 0x00000400,
    GBI_GEOM_CULL_FRONT         = 0x00001000,
    GBI_GEOM_CULL_BACK          = 0x00002000,
    GBI_GEOM_FOG                = 0x00010000,
    GBI_GEOM_LIGHTING           = 0x00020000,
    GBI_GEOM_TEXTURE_GEN        = 0x00040000,
    GBI_GEOM_TEXTURE_GEN_LINEAR = 0x00080000,
    // F3DEX's value (F3DEX2 uses 0x00200000).
    GBI_GEOM_SHADING_SMOOTH     = 0x00000200,
    GBI_GEOM_CLIPPING           = 0x00800000,
};

uint32_t gbi_command_word_length(uint8_t opcode);

const char* gbi_opcode_name(uint8_t opcode);

const char* gbi_image_format_name(uint8_t fmt, uint8_t siz);

struct GbiCensus {
    // --- walk statistics -----------------------------------------------------
    uint64_t commands = 0;      // commands visited
    uint64_t nested_lists = 0;  // G_DL/G_BRANCH_Z targets followed
    uint32_t max_depth = 0;     // deepest G_DL nesting reached

    bool truncated = false;
    const char* truncation_reason = nullptr;
    uint32_t truncation_address = 0;

    // The histogram. Every opcode is recorded, named or not.
    uint32_t opcode_count[256] = {};
    uint32_t unknown_opcode_count = 0;  // opcodes with no name (a decode error)

    uint32_t matrix_params[256] = {};     // bits 16..23
    uint32_t matrix_params_lo[256] = {};  // bits 0..7
    uint32_t matrix_static = 0;         // ... with G_MTX_LOAD (a fresh matrix)
    uint32_t matrix_multiply = 0;       // ... multiplied into the top of stack
    uint32_t matrix_projection = 0;     // ... addressed to the projection stack
    uint32_t matrix_push = 0;           // ... pushed (as opposed to replaced)
    uint32_t pop_matrix = 0;
    uint32_t movemem_index[256] = {};   // G_MOVEMEM index
    uint32_t moveword_index[256] = {};  // G_MOVEWORD index
    uint32_t segment_commands = 0;      // ... of which G_MW_SEGMENT
    uint32_t force_matrix_words = 0;    // MV_MATRIX_1 forms seen, by word count
    uint32_t force_matrix_one_word = 0;
    uint32_t force_matrix_four_words = 0;

    // Vertices loaded, and the distribution of G_VTX batch sizes (n is 6 bits).
    uint64_t vertices = 0;
    uint32_t vertex_batch[65] = {};

    // --- geometry ------------------------------------------------------------
    uint32_t geom_set[32] = {};
    uint32_t geom_clear[32] = {};  // ... and cleared

    // --- textures ------------------------------------------------------------
    uint32_t texture_commands = 0;
    uint32_t texture_on = 0;       // ... with G_TEXTURE_ENABLE
    uint32_t texture_tile[8] = {}; // ... per tile
    uint32_t texture_level[8] = {};
    uint32_t texture_on_value[256] = {};  // ... by the enable byte, verbatim
    uint32_t tile_fmt_siz[8][4] = {};   // G_SETTILE (fmt, siz)
    uint32_t tile_index[8] = {};        // G_SETTILE tile
    uint32_t tile_line_nonzero = 0;     // ... with a non-zero line length
    uint32_t timg_fmt_siz[8][4] = {};   // G_SETTIMG (fmt, siz)
    uint32_t cimg_fmt_siz[8][4] = {};   // G_SETCIMG (fmt, siz)
    uint32_t zimg_commands = 0;         // G_SETZIMG
    uint32_t load_block = 0, load_tile = 0, load_tlut = 0;

    struct ValueCount {
        uint64_t value;
        uint32_t count;
    };
    static constexpr uint32_t MaxDistinct = 256;
    ValueCount combine[MaxDistinct] = {};
    uint32_t combine_count = 0;
    ValueCount othermode_h[MaxDistinct] = {};
    uint32_t othermode_h_count = 0;
    ValueCount othermode_l[MaxDistinct] = {};
    uint32_t othermode_l_count = 0;

    // --- primitives ----------------------------------------------------------
    uint64_t triangles = 0;   // G_TRI1 (1) + G_TRI2 (2) + G_QUAD (2)
    uint64_t texrects = 0;
    uint64_t fillrects = 0;
    uint32_t scissor_commands = 0;

    void record_combine(uint64_t value);
    void record_othermode_h(uint32_t value);
    void record_othermode_l(uint32_t value);

    void print(FILE* out) const;
};

void gbi_census_display_list(uint8_t* rdram, uint32_t address, GbiCensus& census);

constexpr uint32_t GbiRdramSize = 8u * 1024u * 1024u;

}  // namespace wetter::front

#endif  // WETTER_FRONT_GBI_H
