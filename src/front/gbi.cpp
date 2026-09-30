// F3DEX (GBI1) command names and the display-list census.

#include "gbi.h"

#include <algorithm>
#include <cstring>

namespace wetter::front {

namespace {

bool address_in_rdram(uint32_t address) {
    return address <= GbiRdramSize - 8;
}

uint32_t read_word(const uint8_t* rdram, uint32_t address) {
    uint32_t value;
    std::memcpy(&value, rdram + address, sizeof(value));
    return value;
}

}  // namespace

uint32_t gbi_command_word_length(uint8_t opcode) {
    switch (opcode) {
        case GBI_TEXRECT:
        case GBI_TEXRECTFLIP:
            return 2;
        default:
            return 1;
    }
}

const char* gbi_opcode_name(uint8_t opcode) {
    switch (opcode) {
        case GBI_SPNOOP:            return "G_SPNOOP";
        case GBI_MTX:               return "G_MTX";
        case GBI_MOVEMEM:           return "G_MOVEMEM";
        case GBI_VTX:               return "G_VTX";
        case GBI_DL:                return "G_DL";
        case GBI_SPRITE2D_BASE:     return "G_SPRITE2D_BASE";

        case GBI_LOAD_UCODE:        return "G_LOAD_UCODE";
        case GBI_BRANCH_Z:          return "G_BRANCH_Z";
        case GBI_TRI2:              return "G_TRI2";
        case GBI_MODIFYVTX:         return "G_MODIFYVTX";
        case GBI_RDPHALF_2:         return "G_RDPHALF_2";
        case GBI_RDPHALF_1:         return "G_RDPHALF_1";
        case GBI_QUAD:              return "G_QUAD";
        case GBI_CLEARGEOMETRYMODE: return "G_CLEARGEOMETRYMODE";
        case GBI_SETGEOMETRYMODE:   return "G_SETGEOMETRYMODE";
        case GBI_ENDDL:             return "G_ENDDL";
        case GBI_SETOTHERMODE_L:    return "G_SETOTHERMODE_L";
        case GBI_SETOTHERMODE_H:    return "G_SETOTHERMODE_H";
        case GBI_TEXTURE:           return "G_TEXTURE";
        case GBI_MOVEWORD:          return "G_MOVEWORD";
        case GBI_POPMTX:            return "G_POPMTX";
        case GBI_CULLDL:            return "G_CULLDL";
        case GBI_TRI1:              return "G_TRI1";

        case GBI_RDPNOOP:           return "G_RDPNOOP";
        case GBI_TEXRECT:           return "G_TEXRECT";
        case GBI_TEXRECTFLIP:       return "G_TEXRECTFLIP";
        case GBI_RDPLOADSYNC:       return "G_RDPLOADSYNC";
        case GBI_RDPPIPESYNC:       return "G_RDPPIPESYNC";
        case GBI_RDPTILESYNC:       return "G_RDPTILESYNC";
        case GBI_RDPFULLSYNC:       return "G_RDPFULLSYNC";
        case GBI_SETKEYGB:          return "G_SETKEYGB";
        case GBI_SETKEYR:           return "G_SETKEYR";
        case GBI_SETCONVERT:        return "G_SETCONVERT";
        case GBI_SETSCISSOR:        return "G_SETSCISSOR";
        case GBI_SETPRIMDEPTH:      return "G_SETPRIMDEPTH";
        case GBI_RDPSETOTHERMODE:   return "G_RDPSETOTHERMODE";
        case GBI_LOADTLUT:          return "G_LOADTLUT";
        case GBI_SETTILESIZE:       return "G_SETTILESIZE";
        case GBI_LOADBLOCK:         return "G_LOADBLOCK";
        case GBI_LOADTILE:          return "G_LOADTILE";
        case GBI_SETTILE:           return "G_SETTILE";
        case GBI_FILLRECT:          return "G_FILLRECT";
        case GBI_SETFILLCOLOR:      return "G_SETFILLCOLOR";
        case GBI_SETFOGCOLOR:       return "G_SETFOGCOLOR";
        case GBI_SETBLENDCOLOR:     return "G_SETBLENDCOLOR";
        case GBI_SETPRIMCOLOR:      return "G_SETPRIMCOLOR";
        case GBI_SETENVCOLOR:       return "G_SETENVCOLOR";
        case GBI_SETCOMBINE:        return "G_SETCOMBINE";
        case GBI_SETTIMG:           return "G_SETTIMG";
        case GBI_SETZIMG:           return "G_SETZIMG";
        case GBI_SETCIMG:           return "G_SETCIMG";
        default:                    return nullptr;
    }
}

const char* gbi_image_format_name(uint8_t fmt, uint8_t siz) {
    static const char* const format_names[8] = {
        "RGBA", "YUV", "CI", "IA", "I", "fmt5", "fmt6", "fmt7",
    };
    static const char* const size_names[4] = { "4", "8", "16", "32" };

    // One shared buffer: the caller is expected to use the result before asking
    // for another one, which is all this is ever used for.
    static thread_local char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%s%s", format_names[fmt & 7], size_names[siz & 3]);
    return buffer;
}

namespace {

void record_value(GbiCensus::ValueCount* table, uint32_t& count, uint64_t value) {
    for (uint32_t i = 0; i < count; ++i) {
        if (table[i].value == value) {
            ++table[i].count;
            return;
        }
    }

    if (count < GbiCensus::MaxDistinct) {
        table[count].value = value;
        table[count].count = 1;
        ++count;
    }
}

}  // namespace

void GbiCensus::record_combine(uint64_t value) {
    record_value(combine, combine_count, value);
}

void GbiCensus::record_othermode_h(uint32_t value) {
    record_value(othermode_h, othermode_h_count, value);
}

void GbiCensus::record_othermode_l(uint32_t value) {
    record_value(othermode_l, othermode_l_count, value);
}

void gbi_census_display_list(uint8_t* rdram, uint32_t address, GbiCensus& census) {
    constexpr unsigned MaxDepth = 16;
    constexpr uint64_t MaxCommands = 8u << 20;

    uint32_t segments[16];
    for (unsigned i = 0; i < 16; ++i) {
        segments[i] = i << 24;
    }

    auto resolve_address = [&segments](uint32_t seg_address) -> uint32_t {
        return (segments[(seg_address >> 24) & 0x0F] + (seg_address & 0x00FFFFFF)) & 0x00FFFFF8;
    };

    auto stop = [&census](const char* reason, uint32_t at) {
        census.truncated = true;
        if (census.truncation_reason == nullptr) {
            census.truncation_reason = reason;
            census.truncation_address = at;
        }
    };

    address = resolve_address(address);
    if (!address_in_rdram(address)) {
        stop("the display list address is outside RDRAM", address);
        return;
    }

    uint32_t next[MaxDepth] = {};
    unsigned depth = 1;
    next[0] = address;

    uint64_t commands = 0;

    while (depth > 0) {
        if (commands >= MaxCommands) {
            stop("the command limit was reached (the list is probably a loop)",
                 next[depth - 1]);
            break;
        }

        const uint32_t pc = next[depth - 1];
        if (!address_in_rdram(pc)) {
            stop("a list address is outside RDRAM", pc);
            break;
        }

        const uint32_t w0 = read_word(rdram, pc);
        const uint32_t w1 = read_word(rdram, pc + 4);
        const uint8_t opcode = static_cast<uint8_t>(w0 >> 24);

        ++commands;
        ++census.commands;
        if (census.opcode_count[opcode]++ == 0 && gbi_opcode_name(opcode) == nullptr) {
            ++census.unknown_opcode_count;
        }

        uint32_t word_length = gbi_command_word_length(opcode);
        bool finished = false;   // set by the commands that end a list

        switch (opcode) {
            case GBI_ENDDL:
                --depth;
                finished = true;
                break;

            case GBI_DL: {
                const uint32_t target = resolve_address(w1);
                const bool branch = ((w0 >> 16) & 1u) != 0;
                ++census.nested_lists;

                if (branch) {
                    // "jump", not "call": no return address, the current frame
                    // simply continues at the target.
                    next[depth - 1] = target;
                    finished = true;
                } else if (depth < MaxDepth) {
                    next[depth - 1] = pc + word_length * 8;
                    next[depth] = target;
                    ++depth;
                    census.max_depth = std::max(census.max_depth, depth);
                    finished = true;
                }
                break;
            }

            case GBI_BRANCH_Z: {
                const uint32_t target = resolve_address(w1);
                ++census.nested_lists;
                if (depth < MaxDepth) {
                    next[depth - 1] = pc + word_length * 8;
                    next[depth] = target;
                    ++depth;
                    census.max_depth = std::max(census.max_depth, depth);
                    finished = true;
                } else {
                    stop("the display list nesting limit was reached", pc);
                }
                break;
            }

            case GBI_LOAD_UCODE: {
                break;
            }

            case GBI_MTX: {
                const uint8_t params = static_cast<uint8_t>((w0 >> 16) & 0xFFu);
                ++census.matrix_params[params];
                ++census.matrix_params_lo[w0 & 0xFFu];
                if ((params & GBI_MTX_LOAD) != 0) {
                    ++census.matrix_static;
                } else {
                    ++census.matrix_multiply;
                }
                if ((params & GBI_MTX_PROJECTION) != 0) {
                    ++census.matrix_projection;
                }
                if ((params & GBI_MTX_PUSH) != 0) {
                    ++census.matrix_push;
                }
                break;
            }

            case GBI_POPMTX:
                ++census.pop_matrix;
                break;

            case GBI_MOVEMEM: {
                const uint8_t index = static_cast<uint8_t>((w0 >> 16) & 0xFFu);
                ++census.movemem_index[index];

                if (index == GBI_MV_MATRIX_1) {
                    const uint32_t following = pc + 8;
                    const bool run_of_four =
                        address_in_rdram(following) &&
                        static_cast<uint8_t>(read_word(rdram, following) >> 24) == GBI_MOVEMEM &&
                        static_cast<uint8_t>((read_word(rdram, following) >> 16) & 0xFFu) == GBI_MV_MATRIX_2;
                    if (run_of_four) {
                        word_length = 4;
                        ++census.force_matrix_four_words;
                    } else {
                        ++census.force_matrix_one_word;
                    }
                }
                break;
            }

            case GBI_MOVEWORD: {
                const uint8_t index = static_cast<uint8_t>(w0 & 0xFFu);
                ++census.moveword_index[index];

                if (index == GBI_MW_SEGMENT) {
                    const uint32_t segment = (w0 >> 10) & 0xFu;
                    segments[segment] = w1;
                    ++census.segment_commands;
                }
                break;
            }

            case GBI_VTX: {
                const uint32_t count = (w0 >> 10) & 0x3Fu;
                census.vertices += count;
                ++census.vertex_batch[std::min(count, 64u)];
                break;
            }

            case GBI_SETGEOMETRYMODE:
            case GBI_CLEARGEOMETRYMODE: {
                uint32_t* table = (opcode == GBI_SETGEOMETRYMODE) ? census.geom_set
                                                                  : census.geom_clear;
                for (unsigned bit = 0; bit < 32; ++bit) {
                    if ((w1 & (1u << bit)) != 0) {
                        ++table[bit];
                    }
                }
                break;
            }

            case GBI_TEXTURE: {
                const uint32_t tile = (w0 >> 8) & 0x7u;
                const uint32_t level = (w0 >> 11) & 0x7u;
                const uint32_t on = w0 & 0xFFu;
                ++census.texture_commands;
                ++census.texture_tile[tile];
                ++census.texture_level[level];
                ++census.texture_on_value[on];
                if (on != 0) {
                    ++census.texture_on;
                }
                break;
            }

            case GBI_SETTILE: {
                const uint8_t fmt = static_cast<uint8_t>((w0 >> 21) & 0x7u);
                const uint8_t siz = static_cast<uint8_t>((w0 >> 19) & 0x3u);
                const uint32_t line = (w0 >> 9) & 0x1FFu;
                const uint32_t tile = (w1 >> 24) & 0x7u;
                ++census.tile_fmt_siz[fmt][siz];
                ++census.tile_index[tile];
                if (line != 0) {
                    ++census.tile_line_nonzero;
                }
                break;
            }

            case GBI_SETTIMG: {
                const uint8_t fmt = static_cast<uint8_t>((w0 >> 21) & 0x7u);
                const uint8_t siz = static_cast<uint8_t>((w0 >> 19) & 0x3u);
                ++census.timg_fmt_siz[fmt][siz];
                break;
            }

            case GBI_SETCIMG: {
                const uint8_t fmt = static_cast<uint8_t>((w0 >> 21) & 0x7u);
                const uint8_t siz = static_cast<uint8_t>((w0 >> 19) & 0x3u);
                ++census.cimg_fmt_siz[fmt][siz];
                break;
            }

            case GBI_SETZIMG:
                ++census.zimg_commands;
                break;

            case GBI_LOADBLOCK:
                ++census.load_block;
                break;
            case GBI_LOADTILE:
                ++census.load_tile;
                break;
            case GBI_LOADTLUT:
                ++census.load_tlut;
                break;

            case GBI_SETCOMBINE: {
                census.record_combine((static_cast<uint64_t>(w1 & 0x00FFFFFFu) << 24) |
                                      (w0 & 0x00FFFFFFu));
                break;
            }

            case GBI_SETOTHERMODE_H:
            case GBI_SETOTHERMODE_L: {
                if (opcode == GBI_SETOTHERMODE_H) {
                    census.record_othermode_h(w1);
                } else {
                    census.record_othermode_l(w1);
                }
                break;
            }

            case GBI_SETSCISSOR:
                ++census.scissor_commands;
                break;

            case GBI_TRI1:
                ++census.triangles;
                break;
            case GBI_TRI2:
                census.triangles += 2;
                break;
            case GBI_QUAD:
                census.triangles += 2;
                break;

            case GBI_TEXRECT:
            case GBI_TEXRECTFLIP:
                ++census.texrects;
                break;

            case GBI_FILLRECT:
                ++census.fillrects;
                break;

            default:
                break;
        }

        if (!finished) {
            next[depth - 1] = pc + word_length * 8;
        }
    }
}

namespace {

struct NamedCount {
    const char* name;
    uint32_t count;
};

void print_histogram(FILE* out, const char* title, NamedCount* entries, size_t count) {
    std::sort(entries, entries + count,
              [](const NamedCount& a, const NamedCount& b) { return a.count > b.count; });

    bool printed_title = false;
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].count == 0) {
            continue;
        }
        if (!printed_title) {
            fprintf(out, "\n  %s\n", title);
            printed_title = true;
        }
        fprintf(out, "    %-24s %10u\n", entries[i].name, entries[i].count);
    }
}

}  // namespace

void GbiCensus::print(FILE* out) const {
    fprintf(out, "\n[gbi] ===================== display list census =====================\n");
    fprintf(out, "[gbi] commands            %llu\n", static_cast<unsigned long long>(commands));
    fprintf(out, "[gbi] nested lists        %llu (max depth %u)\n",
            static_cast<unsigned long long>(nested_lists), max_depth);
    fprintf(out, "[gbi] triangles           %llu\n", static_cast<unsigned long long>(triangles));
    fprintf(out, "[gbi] texrects            %llu\n", static_cast<unsigned long long>(texrects));
    fprintf(out, "[gbi] fillrects           %llu\n", static_cast<unsigned long long>(fillrects));
    fprintf(out, "[gbi] vertices            %llu\n", static_cast<unsigned long long>(vertices));

    if (truncated) {
        fprintf(out, "[gbi] WARNING: the walk stopped early: %s\n",
                truncation_reason != nullptr ? truncation_reason : "(no reason recorded)");
        fprintf(out, "[gbi]          at RDRAM 0x%06X. The numbers below are a lower bound,\n",
                truncation_address);
        fprintf(out, "[gbi]          not a description of the frame.\n");
    }
    if (unknown_opcode_count != 0) {
        fprintf(out, "[gbi] NOTE: %u opcode(s) are not in this GBI's table. They are\n"
                     "[gbi]       listed below with their counts. Each is either a\n"
                     "[gbi]       command the table is missing or a sign that part of\n"
                     "[gbi]       the stream is being decoded at the wrong offset.\n",
                unknown_opcode_count);
    }

    // --- commands ------------------------------------------------------------
    {
        static NamedCount entries[256];
        static char unknown_names[256][16];
        size_t used = 0;
        for (unsigned opcode = 0; opcode < 256; ++opcode) {
            if (opcode_count[opcode] == 0) {
                continue;
            }
            const char* name = gbi_opcode_name(static_cast<uint8_t>(opcode));
            if (name == nullptr) {
                std::snprintf(unknown_names[opcode], sizeof(unknown_names[opcode]),
                              "?? 0x%02X", opcode);
                name = unknown_names[opcode];
            }
            entries[used++] = NamedCount{ name, opcode_count[opcode] };
        }
        print_histogram(out, "commands, by opcode", entries, used);
    }

    // --- transform -----------------------------------------------------------
    {
        static NamedCount entries[256];
        size_t used = 0;
        for (unsigned i = 0; i < 256; ++i) {
            if (matrix_params[i] == 0) {
                continue;
            }
            static char names[256][24];
            std::snprintf(names[i], sizeof(names[i]), "G_MTX params 0x%02X", i);
            entries[used++] = NamedCount{ names[i], matrix_params[i] };
        }
        print_histogram(out, "G_MTX, by bits 16..23", entries, used);

        used = 0;
        for (unsigned i = 0; i < 256; ++i) {
            if (matrix_params_lo[i] == 0) {
                continue;
            }
            static char low_names[256][24];
            std::snprintf(low_names[i], sizeof(low_names[i]), "G_MTX low byte 0x%02X", i);
            entries[used++] = NamedCount{ low_names[i], matrix_params_lo[i] };
        }
        print_histogram(out, "G_MTX, by bits 0..7", entries, used);

        fprintf(out, "\n  matrix stack\n");
        fprintf(out, "    %-24s %10u\n", "push", matrix_push);
        fprintf(out, "    %-24s %10u\n", "multiply", matrix_multiply);
        fprintf(out, "    %-24s %10u\n", "load (replace)", matrix_static);
        fprintf(out, "    %-24s %10u\n", "to projection stack", matrix_projection);
        fprintf(out, "    %-24s %10u\n", "G_POPMTX", pop_matrix);

        static NamedCount moves[256];
        used = 0;
        for (unsigned i = 0; i < 256; ++i) {
            if (movemem_index[i] == 0) {
                continue;
            }
            static char names[256][24];
            std::snprintf(names[i], sizeof(names[i]), "index 0x%02X", i);
            moves[used++] = NamedCount{ names[i], movemem_index[i] };
        }
        print_histogram(out, "G_MOVEMEM, by index", moves, used);

        used = 0;
        for (unsigned i = 0; i < 256; ++i) {
            if (moveword_index[i] == 0) {
                continue;
            }
            static char names[256][24];
            std::snprintf(names[i], sizeof(names[i]), "index 0x%02X", i);
            moves[used++] = NamedCount{ names[i], moveword_index[i] };
        }
        print_histogram(out, "G_MOVEWORD, by index", moves, used);
        fprintf(out, "    %-24s %10u\n", "...of which G_MW_SEGMENT", segment_commands);

        fprintf(out, "    %-24s %10u (one word)\n", "G_MOVEMEM MV_MATRIX_1",
                force_matrix_one_word);
        fprintf(out, "    %-24s %10u (four words)\n", "G_MOVEMEM MV_MATRIX_1",
                force_matrix_four_words);

        static NamedCount batches[65];
        used = 0;
        for (unsigned n = 0; n <= 64; ++n) {
            if (vertex_batch[n] == 0) {
                continue;
            }
            static char names[65][24];
            std::snprintf(names[n], sizeof(names[n]), "%u vertices", n);
            batches[used++] = NamedCount{ names[n], vertex_batch[n] };
        }
        print_histogram(out, "G_VTX, by batch size", batches, used);
    }

    // --- geometry mode -------------------------------------------------------
    {
        static const struct {
            uint32_t bit;
            const char* name;
        } known[] = {
            { GBI_GEOM_ZBUFFER,            "G_ZBUFFER" },
            { GBI_GEOM_SHADE,              "G_SHADE" },
            { GBI_GEOM_CULL_FRONT,         "G_CULL_FRONT (F3DEX)" },
            { GBI_GEOM_CULL_BACK,          "G_CULL_BACK (F3DEX)" },
            { GBI_GEOM_CULL_FRONT_F3D,     "G_CULL_FRONT (F3D)" },
            { GBI_GEOM_CULL_BACK_F3D,      "G_CULL_BACK (F3D)" },
            { GBI_GEOM_FOG,                "G_FOG" },
            { GBI_GEOM_LIGHTING,           "G_LIGHTING" },
            { GBI_GEOM_TEXTURE_GEN,        "G_TEXTURE_GEN" },
            { GBI_GEOM_TEXTURE_GEN_LINEAR, "G_TEXTURE_GEN_LINEAR" },
            { GBI_GEOM_SHADING_SMOOTH,     "G_SHADING_SMOOTH" },
            { GBI_GEOM_CLIPPING,           "G_CLIPPING" },
        };

        bool printed_title = false;
        for (const auto& entry : known) {
            unsigned bit = 0;
            while ((1u << bit) != entry.bit) {
                ++bit;
            }
            if (geom_set[bit] == 0 && geom_clear[bit] == 0) {
                continue;
            }
            if (!printed_title) {
                fprintf(out, "\n  geometry mode\n");
                printed_title = true;
            }
            fprintf(out, "    %-24s set %10u  cleared %10u\n", entry.name, geom_set[bit],
                    geom_clear[bit]);
        }
        for (unsigned bit = 0; bit < 32; ++bit) {
            if ((geom_set[bit] | geom_clear[bit]) == 0) {
                continue;
            }
            bool named = false;
            for (const auto& entry : known) {
                if (entry.bit == (1u << bit)) {
                    named = true;
                }
            }
            if (!named) {
                fprintf(out, "    %-24s set %10u  cleared %10u\n", "bit ?", geom_set[bit],
                        geom_clear[bit]);
                fprintf(out, "      (bit %u, value 0x%08X)\n", bit, 1u << bit);
            }
        }
    }

    // --- textures ------------------------------------------------------------
    {
        fprintf(out, "\n  textures\n");
        fprintf(out, "    %-24s %10u\n", "G_TEXTURE", texture_commands);
        fprintf(out, "    %-24s %10u\n", "  with texture enabled", texture_on);
        for (unsigned value = 0; value < 256; ++value) {
            if (texture_on_value[value] == 0) {
                continue;
            }
            fprintf(out, "      enable byte 0x%02X x%u\n", value, texture_on_value[value]);
        }
        fprintf(out, "    %-24s %10u\n", "G_LOADBLOCK", load_block);
        fprintf(out, "    %-24s %10u\n", "G_LOADTILE", load_tile);
        fprintf(out, "    %-24s %10u\n", "G_LOADTLUT", load_tlut);
        fprintf(out, "    %-24s %10u\n", "G_SETTILE line != 0", tile_line_nonzero);

        for (unsigned fmt = 0; fmt < 8; ++fmt) {
            for (unsigned siz = 0; siz < 4; ++siz) {
                if (tile_fmt_siz[fmt][siz] != 0) {
                    fprintf(out, "    %-24s %10u\n", gbi_image_format_name(fmt, siz),
                            tile_fmt_siz[fmt][siz]);
                }
            }
        }

        bool printed_timg = false;
        for (unsigned fmt = 0; fmt < 8; ++fmt) {
            for (unsigned siz = 0; siz < 4; ++siz) {
                if (timg_fmt_siz[fmt][siz] == 0) {
                    continue;
                }
                if (!printed_timg) {
                    fprintf(out, "\n    G_SETTIMG formats\n");
                    printed_timg = true;
                }
                fprintf(out, "      %-22s %10u\n", gbi_image_format_name(fmt, siz),
                        timg_fmt_siz[fmt][siz]);
            }
        }

        bool printed_cimg = false;
        for (unsigned fmt = 0; fmt < 8; ++fmt) {
            for (unsigned siz = 0; siz < 4; ++siz) {
                if (cimg_fmt_siz[fmt][siz] == 0) {
                    continue;
                }
                if (!printed_cimg) {
                    fprintf(out, "\n    G_SETCIMG formats\n");
                    printed_cimg = true;
                }
                fprintf(out, "      %-22s %10u\n", gbi_image_format_name(fmt, siz),
                        cimg_fmt_siz[fmt][siz]);
            }
        }
    }

    // --- RDP state -----------------------------------------------------------
    fprintf(out, "\n  RDP state\n");
    fprintf(out, "    %-24s %10u distinct values\n", "G_SETCOMBINE", combine_count);
    for (uint32_t i = 0; i < combine_count; ++i) {
        fprintf(out, "      0x%014llX  x%u\n",
                static_cast<unsigned long long>(combine[i].value), combine[i].count);
    }
    fprintf(out, "    %-24s %10u distinct values\n", "G_SETOTHERMODE_H", othermode_h_count);
    for (uint32_t i = 0; i < othermode_h_count; ++i) {
        fprintf(out, "      0x%08llX  x%u\n",
                static_cast<unsigned long long>(othermode_h[i].value), othermode_h[i].count);
    }
    fprintf(out, "    %-24s %10u distinct values\n", "G_SETOTHERMODE_L", othermode_l_count);
    for (uint32_t i = 0; i < othermode_l_count; ++i) {
        fprintf(out, "      0x%08llX  x%u\n",
                static_cast<unsigned long long>(othermode_l[i].value), othermode_l[i].count);
    }

    fprintf(out, "    %-24s %10u\n", "G_SETZIMG", zimg_commands);
    fprintf(out, "    %-24s %10u\n", "G_SETSCISSOR", scissor_commands);

    fprintf(out, "[gbi] ==============================================================\n");
    fflush(out);
}

}  // namespace wetter::front
