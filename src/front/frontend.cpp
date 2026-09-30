// front end: the display-list walker. See frontend.h.

#include "frontend.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gbi.h"

namespace wetter::front {

namespace {

constexpr unsigned MaxListDepth = 16;
constexpr uint64_t MaxCommandsPerList = 1u << 22;

// G_MOVEMEM indices this interpreter handles.
enum : uint32_t {
    MV_VIEWPORT = 0x80,
    MV_LOOKATY = 0x82,
    MV_LOOKATX = 0x84,
    MV_LIGHT_0 = 0x86,
    MV_LIGHT_7 = 0x94,
    MV_MATRIX_1 = 0x9E,
};

}  // namespace

Frontend::Frontend(uint8_t* rdram, RdpSink& rdp, bool force_branch_z, float x_extent)
    : rdram_(rdram), rdp_(rdp), force_branch_z_(force_branch_z) {
    rsp_.set_x_extent(x_extent);
    for (unsigned i = 0; i < 16; ++i) segments_[i] = i << 24;
    // The transform pipeline reads vertices and matrices straight out of the
    // game's memory, so it is handed the same buffer the RDP has.
    rsp_.set_rdram(rdram);
}

void Frontend::set_rdram(uint8_t* rdram) {
    rdram_ = rdram;
    rsp_.set_rdram(rdram);
}

void Frontend::set_log_window(bool open, bool geometry_log) {
    log_window_open_ = open;
    rsp_.set_geo_log(geometry_log && open);
    rsp_.set_log_window(open);
}

uint32_t Frontend::resolve(uint32_t address) const {
    return (segments_[(address >> 24) & 0x0F] + (address & 0x00FFFFFFu)) & 0x00FFFFF8u;
}

uint32_t Frontend::read_word(uint32_t address) const {
    if (address + 4u > RdramSize) return 0;
    uint32_t value;
    std::memcpy(&value, rdram_ + address, sizeof(value));
    return value;
}

void Frontend::run(uint32_t address) {
    uint32_t next[MaxListDepth] = {};
    unsigned depth = 1;
    next[0] = resolve(address);

    uint64_t commands = 0;

    static const bool cmd_log = std::getenv("WETTER_CMD_LOG") != nullptr;
    static unsigned cmd_logged = 0;
    static const bool vtx_log_enabled = std::getenv("WETTER_VTX_LOG") != nullptr;

    while (depth > 0 && commands < MaxCommandsPerList) {
        const uint32_t pc = next[depth - 1];
        if (pc + 8u > RdramSize) break;

        ++commands;

        const uint32_t w0 = read_word(pc);
        const uint32_t w1 = read_word(pc + 4);
        const uint8_t opcode = static_cast<uint8_t>(w0 >> 24);

        if (cmd_log && log_window_open_ && cmd_logged < 400u) {
            ++cmd_logged;
            std::fprintf(stderr, "[cmd] %03llu pc 0x%06X op %02X w0 %08X w1 %08X\n", (unsigned long long)commands, pc,
                         opcode, w0, w1);
        }

        uint32_t advance = pc + 8u;
        bool handled = false;   // the command set the next address itself

        switch (opcode) {
            case GBI_ENDDL:
                --depth;
                handled = true;
                break;

            case GBI_DL: {
                const uint32_t target = resolve(w1);
                const bool branch = ((w0 >> 16) & 1u) != 0;
                if (branch) {
                    next[depth - 1] = target;
                    handled = true;
                } else if (depth < MaxListDepth) {
                    next[depth - 1] = advance;
                    next[depth] = target;
                    ++depth;
                    handled = true;
                }
                break;
            }

            case GBI_RDPHALF_1:
                rdphalf1_ = w1;
                break;
            case GBI_BRANCH_Z: {
                // gSPBranchLessZ: go to G_RDPHALF_1's list when the vertex is
                // nearer than zval (w1, as G_DEPTOZS encodes it).
                const bool take = force_branch_z_ || rsp_.screen_z_fixed((w0 & 0xFFFu) / 2u) <= int64_t(w1);
                if (take) {
                    next[depth - 1] = resolve(rdphalf1_);
                    handled = true;
                }
                break;
            }

            case GBI_MTX:
                rsp_.matrix(w1, (w0 >> 16) & 0xFFu);
                break;

            case GBI_MOVEMEM: {
                const uint32_t index = (w0 >> 16) & 0xFFu;
                if (index == MV_VIEWPORT) {
                    rsp_.set_viewport(w1);
                } else if (index >= MV_LIGHT_0 && index <= MV_LIGHT_7 && ((index - MV_LIGHT_0) % 2u) == 0u) {
                    rsp_.set_light((index - MV_LIGHT_0) / 2u, w1);
                } else if (index == MV_LOOKATX) {
                    rsp_.set_look_at(0, w1);
                } else if (index == MV_LOOKATY) {
                    rsp_.set_look_at(1, w1);
                } else if (index == MV_MATRIX_1) {
                    rsp_.force_matrix(w1);
                    advance = pc + 32u;
                }
                break;
            }

            case GBI_MOVEWORD: {
                const uint32_t index = w0 & 0xFFu;
                if (index == GBI_MW_SEGMENT) {
                    const uint32_t segment = (w0 >> 10) & 0xFu;
                    segments_[segment] = w1;
                    rsp_.set_segment(segment, w1);
                } else if (index == GBI_MW_NUMLIGHT) {
                    rsp_.set_light_count(w1 & 0x00FFFFFFu);
                }
                break;
            }

            case GBI_VTX: {
                const uint32_t count = (w0 >> 10) & 0x3Fu;
                const uint32_t dst = (w0 >> 17) & 0x7Fu;
                rsp_.load_vertices(w1, count, dst);

                static const char* vtx_log = std::getenv("WETTER_VTX_LOG");
                static const bool vtx_log_all = vtx_log != nullptr && std::strtoul(vtx_log, nullptr, 10) == 0;
                static const uint64_t vtx_log_frame = vtx_log != nullptr ? std::strtoull(vtx_log, nullptr, 10) : 0ull;
                static unsigned vtx_logged = 0;
                const bool vtx_wanted =
                    vtx_log != nullptr && (vtx_log_all ? vtx_logged < 400u : rdp_.frames() == vtx_log_frame);
                if (vtx_wanted) {
                    ++vtx_logged;
                    std::fprintf(stderr, "[render] vtx %u f%llu: ram 0x%06X count %u dst %u\n", vtx_logged,
                                 (unsigned long long)rdp_.frames(), resolve(w1), count, dst);
                }
                break;
            }

            case GBI_POPMTX:
                rsp_.pop_matrix(w1);
                break;

            // --- primitives --------------------------------------------------
            case GBI_TRI1:
                triangles_waiting_ += 1;
                rsp_.draw_triangle((w1 >> 17) & 0x7Fu, (w1 >> 9) & 0x7Fu, (w1 >> 1) & 0x7Fu, rdp_);
                if (vtx_log_enabled) {
                    static unsigned tri_logged = 0;
                    if (tri_logged < 400) {
                        ++tri_logged;
                        std::fprintf(stderr, "[render] tri1 %u f%llu: %u %u %u\n", tri_logged,
                                     (unsigned long long)rdp_.frames(), (unsigned int)((w1 >> 17) & 0x7Fu),
                                     (unsigned int)((w1 >> 9) & 0x7Fu), (unsigned int)((w1 >> 1) & 0x7Fu));
                    }
                }
                break;
            case GBI_TRI2:
                triangles_waiting_ += 2;
                rsp_.draw_triangle((w0 >> 17) & 0x7Fu, (w0 >> 9) & 0x7Fu, (w0 >> 1) & 0x7Fu, rdp_);
                rsp_.draw_triangle((w1 >> 17) & 0x7Fu, (w1 >> 9) & 0x7Fu, (w1 >> 1) & 0x7Fu, rdp_);
                if (vtx_log_enabled) {
                    static unsigned tri2_logged = 0;
                    if (tri2_logged < 200) {
                        ++tri2_logged;
                        std::fprintf(stderr, "[render] tri2 %u f%llu: %u %u %u | %u %u %u\n", tri2_logged,
                                     (unsigned long long)rdp_.frames(), (unsigned int)((w0 >> 17) & 0x7Fu),
                                     (unsigned int)((w0 >> 9) & 0x7Fu), (unsigned int)((w0 >> 1) & 0x7Fu),
                                     (unsigned int)((w1 >> 17) & 0x7Fu), (unsigned int)((w1 >> 9) & 0x7Fu),
                                     (unsigned int)((w1 >> 1) & 0x7Fu));
                    }
                }
                break;
            case GBI_QUAD:
                triangles_waiting_ += 2;
                rsp_.draw_triangle((w1 >> 25) & 0x7Fu, (w1 >> 17) & 0x7Fu, (w1 >> 9) & 0x7Fu, rdp_);
                rsp_.draw_triangle((w1 >> 25) & 0x7Fu, (w1 >> 9) & 0x7Fu, (w1 >> 1) & 0x7Fu, rdp_);
                break;

            case GBI_FILLRECT: {
                const int32_t ulx = static_cast<int32_t>((w1 >> 12) & 0xFFFu);
                const int32_t uly = static_cast<int32_t>(w1 & 0xFFFu);
                const int32_t lrx = static_cast<int32_t>((w0 >> 12) & 0xFFFu);
                const int32_t lry = static_cast<int32_t>(w0 & 0xFFFu);
                rdp_.fill_rect(ulx, uly, lrx, lry);
                break;
            }

            case GBI_TEXRECT:
            case GBI_TEXRECTFLIP: {
                const int32_t ulx = static_cast<int32_t>((w1 >> 12) & 0xFFFu);
                const int32_t uly = static_cast<int32_t>(w1 & 0xFFFu);
                const int32_t lrx = static_cast<int32_t>((w0 >> 12) & 0xFFFu);
                const int32_t lry = static_cast<int32_t>(w0 & 0xFFFu);
                const uint32_t tile = (w1 >> 24) & 0x7u;

                static const bool words_log = std::getenv("WETTER_RECT_LOG") != nullptr;
                static unsigned words_logged = 0;
                if (words_log && words_logged < 64 && rdp_.frames() == rdp_.traced_frame()) {
                    ++words_logged;
                    std::fprintf(stderr, "[render] texrect words: %08X %08X | %08X %08X | %08X %08X\n", w0, w1,
                                 read_word(pc + 8u), read_word(pc + 12u), read_word(pc + 16u), read_word(pc + 20u));
                    std::fflush(stderr);
                }

                const uint32_t s_t = read_word(pc + 12u);
                const int32_t uls = static_cast<int16_t>((s_t >> 16) & 0xFFFFu);
                const int32_t ult = static_cast<int16_t>(s_t & 0xFFFFu);

                const uint32_t d_s = read_word(pc + 20u);
                const int32_t dsdx = static_cast<int16_t>((d_s >> 16) & 0xFFFFu);
                const int32_t dtdy = static_cast<int16_t>(d_s & 0xFFFFu);

                rdp_.tex_rect(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, opcode == GBI_TEXRECTFLIP);
                advance = pc + 16u;
                break;
            }

            // --- RDP state ----------------------------------------------------
            case GBI_SETCIMG:
                rsp_.set_color_width((w0 & 0xFFFu) + 1u);
                rdp_.set_color_image((w0 >> 21) & 0x7u, (w0 >> 19) & 0x3u, (w0 & 0xFFFu) + 1u, w1);
                break;

            case GBI_SETZIMG:
                rdp_.set_depth_image(w1);
                break;

            case GBI_SETTIMG:
                rdp_.set_texture_image((w0 >> 21) & 0x7u, (w0 >> 19) & 0x3u, (w0 & 0xFFFu) + 1u, w1);
                break;

            case GBI_SETTILE:
                rdp_.set_tile((w0 >> 21) & 0x7u, (w0 >> 19) & 0x3u, (w0 >> 9) & 0x1FFu, w0 & 0x1FFu, (w1 >> 24) & 0x7u,
                              (w1 >> 20) & 0xFu, (w1 >> 18) & 0x3u, (w1 >> 14) & 0xFu, (w1 >> 10) & 0xFu,
                              (w1 >> 8) & 0x3u, (w1 >> 4) & 0xFu, w1 & 0xFu);
                break;

            case GBI_SETTILESIZE:
                rdp_.set_tile_size((w1 >> 24) & 0x7u, (w0 >> 12) & 0xFFFu, w0 & 0xFFFu, (w1 >> 12) & 0xFFFu,
                                   w1 & 0xFFFu);
                break;

            case GBI_LOADTLUT:
                rdp_.load_tlut((w1 >> 24) & 0x7u, (w0 >> 12) & 0xFFFu, w0 & 0xFFFu, (w1 >> 12) & 0xFFFu, w1 & 0xFFFu);
                break;

            case GBI_SETCOMBINE:
                rdp_.set_combine(w0, w1);
                break;

            case GBI_SETOTHERMODE_H:
                rdp_.set_other_mode_h((w0 >> 8) & 0xFFu, w0 & 0xFFu, w1);
                break;

            case GBI_SETOTHERMODE_L:
                rdp_.set_other_mode_l((w0 >> 8) & 0xFFu, w0 & 0xFFu, w1);
                break;

            case GBI_SETGEOMETRYMODE:
                geometry_mode_ |= w1;
                rsp_.set_geometry_mode(w1, 0u);
                rdp_.set_geometry_mode(geometry_mode_);
                break;

            case GBI_CLEARGEOMETRYMODE:
                geometry_mode_ &= ~w1;
                rsp_.set_geometry_mode(0u, w1);
                rdp_.set_geometry_mode(geometry_mode_);
                break;

            case GBI_TEXTURE:
                rsp_.set_texture((w0 >> 8) & 0x7u, (w0 >> 11) & 0x7u, (w0 & 0xFFu) != 0 ? 1u : 0u,
                                 static_cast<int32_t>((w1 >> 16) & 0xFFFFu), static_cast<int32_t>(w1 & 0xFFFFu));
                break;

            case GBI_SETSCISSOR:
                rdp_.set_scissor((w0 >> 12) & 0xFFFu, w0 & 0xFFFu, (w1 >> 12) & 0xFFFu, w1 & 0xFFFu);
                break;

            case GBI_SETENVCOLOR:
                rdp_.set_env_color(w1);
                break;
            case GBI_SETPRIMCOLOR:
                rdp_.set_prim_color(w1);
                break;
            case GBI_SETBLENDCOLOR:
                rdp_.set_blend_color(w1);
                break;
            case GBI_SETFOGCOLOR:
                rdp_.set_fog_color(w1);
                break;
            case GBI_SETFILLCOLOR:
                rdp_.set_fill_color(w1);
                break;
            case GBI_SETPRIMDEPTH:
                rdp_.set_prim_depth(w1);
                break;

            case GBI_LOADBLOCK:
                rdp_.load_block((w1 >> 24) & 0x7u, (w0 >> 12) & 0xFFFu, w0 & 0xFFFu, (w1 >> 12) & 0xFFFu,
                                w1 & 0xFFFu);
                break;
            case GBI_LOADTILE:
                rdp_.load_tile((w1 >> 24) & 0x7u, (w0 >> 12) & 0xFFFu, w0 & 0xFFFu, (w1 >> 12) & 0xFFFu, w1 & 0xFFFu);
                break;

            // The sync commands and G_RDPNOOP need nothing: a backend finishes
            // every draw before the list returns.
            default:
                break;
        }

        if (!handled) {
            next[depth - 1] = advance;
        }
    }
}

}  // namespace wetter::front
