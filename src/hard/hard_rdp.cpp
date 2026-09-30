// hard: the RDP on the GPU. See hard_rdp.h.
//
// The rules follow soft's RDP (the reference): the same texel addressing,
// combiner slots, alpha tests and blender, evaluated per fragment in a shader
// generated for each distinct combiner + blender setup.

#include "hard_rdp.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "front/combine.h"

namespace wetter::hard {

using namespace gl;
using namespace front::cc;

namespace {

constexpr uint32_t CycleShift = 20;
constexpr uint32_t Cycle1 = 0u << CycleShift;
constexpr uint32_t Cycle2 = 1u << CycleShift;
constexpr uint32_t CycleCopy = 2u << CycleShift;
constexpr uint32_t CycleFill = 3u << CycleShift;
constexpr uint32_t ZCompare = 0x10;
constexpr uint32_t ZUpdate = 0x20;
constexpr uint32_t CvgXAlpha = 0x1000;
constexpr uint32_t ForceBlend = 0x4000;

inline float unit8(uint32_t v) { return static_cast<float>(v & 0xFFu) / 255.0f; }
inline uint32_t expand5(uint32_t v) { return ((v & 0x1Fu) << 3) | ((v & 0x1Fu) >> 2); }

inline void set_rgba(float* out, uint32_t rgba) {
    out[0] = unit8(rgba >> 24);
    out[1] = unit8(rgba >> 16);
    out[2] = unit8(rgba >> 8);
    out[3] = unit8(rgba);
}

const char* kVertex =
    "in vec2 a_pos;\n"
    "in float a_depth;\n"
    "in vec4 a_color;\n"
    "in vec3 a_stq;\n"
    "in vec4 a_rect;\n"
    "uniform vec2 u_size;\n"
    "uniform float u_pad;\n"
    "out vec4 v_color;\n"
    "out vec3 v_stq;\n"
    "out vec4 v_rect;\n"
    "void main() {\n"
    // Game x lands u_pad columns into the (possibly wider) target.
    "    vec2 ndc = vec2((a_pos.x + u_pad) / u_size.x * 2.0 - 1.0, 1.0 - a_pos.y / u_size.y * 2.0);\n"
    // w stays 1: shade is interpolated linearly on screen, as on the RDP;
    // texel coordinates carry their own q and are divided per fragment.
    "    gl_Position = vec4(ndc, a_depth * 2.0 - 1.0, 1.0);\n"
    "    v_color = a_color;\n"
    "    v_stq = a_stq;\n"
    "    v_rect = a_rect;\n"
    "}\n";

// A combiner operand as GLSL, in the colour or the alpha channel.
std::string operand(uint8_t s, bool alpha) {
    const char* e = "vec4(0.0)";
    switch (s) {
        case S_COMBINED: e = "comb"; break;
        case S_TEXEL0: e = "t0"; break;
        case S_TEXEL1: e = "t1"; break;
        case S_PRIM: e = "u_prim"; break;
        case S_SHADE: e = "shade"; break;
        case S_ENV: e = "u_env"; break;
        case S_ONE: e = "vec4(1.0)"; break;
        case S_ZERO: e = "vec4(0.0)"; break;
        case S_COMBINED_A: e = "vec4(comb.a)"; break;
        case S_TEXEL0_A: e = "vec4(t0.a)"; break;
        case S_TEXEL1_A: e = "vec4(t1.a)"; break;
        case S_PRIM_A: e = "vec4(u_prim.a)"; break;
        case S_SHADE_A: e = "vec4(shade.a)"; break;
        case S_ENV_A: e = "vec4(u_env.a)"; break;
        default: break;   // LOD fractions: no mipmapping, so zero
    }
    return std::string("(") + e + (alpha ? ").a" : ").rgb");
}

// One cycle of (A - B) * C + D, clamped as the combiner's output is.
std::string cycle_expr(const uint8_t* sel) {
    std::string rgb = "clamp((" + operand(sel[0], false) + " - " + operand(sel[1], false) + ") * " +
                      operand(sel[2], false) + " + " + operand(sel[3], false) + ", 0.0, 1.0)";
    std::string a = "clamp((" + operand(sel[4], true) + " - " + operand(sel[5], true) + ") * " +
                    operand(sel[6], true) + " + " + operand(sel[7], true) + ", 0.0, 1.0)";
    return "vec4(" + rgb + ", " + a + ")";
}

// The blender's P/M input as GLSL (1, memory, is handled by the caller).
const char* blend_colour(uint8_t sel) {
    switch (sel) {
        case 0: return "cur";
        case 2: return "u_blend.rgb";
        default: return "u_fog.rgb";
    }
}

const char* blend_a(uint8_t sel) {
    switch (sel) {
        case 0: return "ca";
        case 1: return "u_fog.a";
        case 2: return "shade.a";
        default: return "0.0";
    }
}

}  // namespace

bool HardRdp::DrawState::operator==(const DrawState& o) const {
    return program == o.program && tex[0] == o.tex[0] && tex[1] == o.tex[1] &&
           std::memcmp(map, o.map, sizeof(map)) == 0 && std::memcmp(prim, o.prim, sizeof(prim)) == 0 &&
           std::memcmp(env, o.env, sizeof(env)) == 0 && std::memcmp(blend, o.blend, sizeof(blend)) == 0 &&
           std::memcmp(fog, o.fog, sizeof(fog)) == 0 && std::memcmp(fill, o.fill, sizeof(fill)) == 0 &&
           persp == o.persp && depth == o.depth && blending == o.blending;
}

HardRdp::HardRdp(uint8_t* rdram, const Api& api, bool gles, int scale, bool copy_back_every_task, float extension)
    : rdram_(rdram), gl_(api), gles_(gles), scale_(std::max(1, scale)), copy_back_every_task_(copy_back_every_task),
      extension_(std::max(1.0f, extension)) {
    tm_.set_rdram(rdram);
}

HardRdp::~HardRdp() {
    for (auto& [address, t] : targets_) {
        gl_.DeleteFramebuffers(1, &t.fbo);
        gl_.DeleteTextures(1, &t.color);
    }
    for (auto& [key, rb] : depth_buffers_) gl_.DeleteRenderbuffers(1, &rb);
    if (cpu_picture_.fbo != 0) {
        gl_.DeleteFramebuffers(1, &cpu_picture_.fbo);
        gl_.DeleteTextures(1, &cpu_picture_.color);
    }
    for (auto& [key, t] : textures_) gl_.DeleteTextures(1, &t.id);
    if (white_ != 0) gl_.DeleteTextures(1, &white_);
}

bool HardRdp::init() {
    gl_.GenVertexArrays(1, &vao_);
    gl_.BindVertexArray(vao_);
    gl_.GenBuffers(1, &vbo_);
    gl_.BindBuffer(ARRAY_BUFFER, vbo_);
    const GLsizei stride = VertexFloats * sizeof(float);
    gl_.VertexAttribPointer(0, 2, FLOAT, FALSE, stride, reinterpret_cast<const void*>(0));
    gl_.VertexAttribPointer(1, 1, FLOAT, FALSE, stride, reinterpret_cast<const void*>(2 * sizeof(float)));
    gl_.VertexAttribPointer(2, 4, FLOAT, FALSE, stride, reinterpret_cast<const void*>(3 * sizeof(float)));
    gl_.VertexAttribPointer(3, 3, FLOAT, FALSE, stride, reinterpret_cast<const void*>(7 * sizeof(float)));
    gl_.VertexAttribPointer(4, 4, FLOAT, FALSE, stride, reinterpret_cast<const void*>(10 * sizeof(float)));
    for (GLuint i = 0; i < 5; ++i) gl_.EnableVertexAttribArray(i);

    const uint32_t white = 0xFFFFFFFFu;
    gl_.GenTextures(1, &white_);
    gl_.BindTexture(TEXTURE_2D, white_);
    gl_.TexImage2D(TEXTURE_2D, 0, RGBA8, 1, 1, 0, RGBA, UNSIGNED_BYTE, &white);
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, NEAREST);
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, NEAREST);

    gl_.Disable(BLEND);
    gl_.Disable(CULL_FACE);
    gl_.Disable(DITHER);
    gl_.Enable(SCISSOR_TEST);
    gl_.DepthFunc(LESS);
    batch_.reserve(4096 * VertexFloats);

    std::fprintf(stderr, "[hard] %s | %s\n", reinterpret_cast<const char*>(gl_.GetString(VERSION)),
                 reinterpret_cast<const char*>(gl_.GetString(RENDERER)));
    std::fflush(stderr);

    // Compile one program now, so a broken GL fails here rather than mid-game.
    bool blending = false;
    return program_for_state(false, blending) != nullptr;
}

// --- programs ------------------------------------------------------------------

HardRdp::Program* HardRdp::program_for_state(bool textured, bool& blending) {
    (void)textured;
    // Everything the generated shader depends on: the combine word, the cycle
    // type, and the other-mode L bits for alpha compare, coverage, blender.
    const uint64_t key = (uint64_t(combine_l_) << 40) ^ (uint64_t(combine_h_) << 8) ^
                         uint64_t((other_mode_h_ >> CycleShift) & 3u) ^
                         (uint64_t(other_mode_l_ & 0xFFFF5003u) * 0x9E3779B97F4A7C15ull);
    if (key != last_state_key_) {
        auto it = program_by_state_.find(key);
        if (it == program_by_state_.end()) it = program_by_state_.emplace(key, build_program()).first;
        last_state_key_ = key;
        last_program_ = it->second;
    }
    blending = last_program_.second;
    return last_program_.first;
}

std::pair<HardRdp::Program*, bool> HardRdp::build_program() {
    bool blending = false;
    const uint32_t cycle = other_mode_h_ & (3u << CycleShift);
    const bool two = cycle == Cycle2;
    const Slots slots = decode(combine_l_, combine_h_, two);
    const bool threshold = (other_mode_l_ & 3u) == 1u;
    const bool cvg_x_alpha = (other_mode_l_ & CvgXAlpha) != 0u;
    const bool force = (other_mode_l_ & ForceBlend) != 0u;
    const uint32_t bits = other_mode_l_ >> 16;
    uint8_t bl[2][4];
    for (int cyc = 0; cyc < 2; ++cyc) {
        const uint32_t sh = cyc == 0 ? 0u : 2u;   // cycle 1 sits two bits below cycle 0
        bl[cyc][0] = (bits >> (14 - sh)) & 3u;
        bl[cyc][1] = (bits >> (10 - sh)) & 3u;
        bl[cyc][2] = (bits >> (6 - sh)) & 3u;
        bl[cyc][3] = (bits >> (2 - sh)) & 3u;
    }
    const int cycles = two ? 2 : 1;

    // The fragment shader, built as a string; the string is the program's key.
    std::string fs;
    fs += "uniform sampler2D u_tex0;\nuniform sampler2D u_tex1;\n";
    fs += "uniform vec4 u_map0;\nuniform vec4 u_map1;\nuniform vec4 u_prim;\nuniform vec4 u_env;\n";
    fs += "uniform vec4 u_blend;\nuniform vec4 u_fog;\nuniform vec4 u_fill;\nuniform float u_persp;\n";
    fs += "uniform vec2 u_size;\nuniform float u_scale;\n";
    fs += "in vec4 v_color;\nin vec3 v_stq;\nin vec4 v_rect;\nout vec4 o_color;\n";
    fs += "void main() {\n";
    fs += "    vec2 st;\n";
    // A texture rectangle steps s/t once per native pixel, from its top-left
    // corner, as the RDP does; interpolating across an upscaled pixel would
    // split it between texels.
    fs += "    if (v_stq.z < 0.0) {\n";
    fs += "        vec2 px = floor(vec2(gl_FragCoord.x, u_size.y * u_scale - gl_FragCoord.y) / u_scale) - v_rect.xy;\n";
    fs += "        if (v_stq.z < -1.5) px = px.yx;\n";
    fs += "        st = v_stq.xy + px * v_rect.zw;\n";
    fs += "    } else {\n";
    fs += "        st = v_stq.xy / v_stq.z * u_persp;\n";
    fs += "    }\n";
    fs += "    vec4 t0 = texture(u_tex0, vec2(st.x * u_map0.x + u_map0.y, st.y * u_map0.z + u_map0.w));\n";
    fs += "    vec4 t1 = texture(u_tex1, vec2(st.x * u_map1.x + u_map1.y, st.y * u_map1.z + u_map1.w));\n";
    fs += "    vec4 shade = v_color;\n";
    fs += "    vec4 comb = vec4(0.0);\n";
    blending = false;
    if (cycle == CycleFill) {
        fs += "    o_color = u_fill;\n}\n";
    } else {
        if (cycle == CycleCopy) {
            fs += "    vec4 c = t0;\n";   // copy mode has no combiner
        } else {
            if (two) fs += "    comb = " + cycle_expr(slots.sel[0]) + ";\n";
            fs += "    vec4 c = " + cycle_expr(slots.sel[1]) + ";\n";
        }
        if (threshold) fs += "    if (c.a < u_blend.a) discard;\n";
        if (cvg_x_alpha && cycle != CycleCopy) fs += "    if (c.a <= 0.188) discard;\n";

        // The blender. Identity setups (every cycle blends the colour with itself)
        // and copy mode pass the colour through.
        bool identity = true;
        for (int cyc = 0; cyc < cycles; ++cyc) {
            const bool blends = cyc < cycles - 1 || force;
            if (!(bl[cyc][0] == 0 && (!blends || bl[cyc][2] == 0))) identity = false;
        }
        fs += "    vec3 cur = c.rgb;\n    float ca = c.a;\n    float out_a = 1.0;\n";
        if (!identity && cycle != CycleCopy) {
            for (int cyc = 0; cyc < cycles; ++cyc) {
                const uint8_t p = bl[cyc][0], a = bl[cyc][1], m = bl[cyc][2], b = bl[cyc][3];
                const bool last = cyc == cycles - 1;
                if (last && !force) {
                    if (p == 1) fs += "    discard;\n";   // covered, P is memory: the pixel keeps what it has
                    else fs += std::string("    cur = ") + blend_colour(p) + ";\n";
                    continue;
                }
                fs += std::string("    { float wa = ") + blend_a(a) + ";\n";
                const bool memory = p == 1 || m == 1 || b == 1;
                if (memory) {
                    if (last && b == 0 && m == 1 && p != 1) {
                        // P * a + memory * (1 - a): the GPU's source-alpha blend.
                        fs += std::string("      cur = ") + blend_colour(p) + "; out_a = wa; }\n";
                        blending = true;
                    } else if (last && b == 0 && p == 1 && m != 1) {
                        fs += std::string("      cur = ") + blend_colour(m) + "; out_a = 1.0 - wa; }\n";
                        blending = true;
                    } else {
                        fs += "    }\n";   // a first cycle reading memory: not modelled
                    }
                    continue;
                }
                const char* wb = b == 0 ? "(1.0 - wa)" : b == 2 ? "1.0" : "0.0";
                fs += std::string("      float wb = ") + wb + ";\n";
                fs += std::string("      cur = (wa + wb) > 0.0 ? (") + blend_colour(p) + " * wa + " + blend_colour(m) +
                      " * wb) / (wa + wb) : " + blend_colour(m) + "; }\n";
            }
        }
        fs += "    o_color = vec4(cur, out_a);\n}\n";
    }

    auto it = programs_.find(fs);
    if (it != programs_.end()) return { &it->second, blending };

    const char* header = gles_ ? "#version 300 es\nprecision highp float;\n" : "#version 330 core\n";
    auto compile = [&](GLenum type, const char* body) -> GLuint {
        const GLuint s = gl_.CreateShader(type);
        const GLchar* parts[2] = { header, body };
        gl_.ShaderSource(s, 2, parts, nullptr);
        gl_.CompileShader(s);
        GLint ok = 0;
        gl_.GetShaderiv(s, COMPILE_STATUS, &ok);
        if (!ok) {
            char log[2048] = {};
            gl_.GetShaderInfoLog(s, sizeof(log), nullptr, log);
            std::fprintf(stderr, "[hard] shader failed: %s\n%s\n", log, body);
            std::fflush(stderr);
            gl_.DeleteShader(s);
            return 0;
        }
        return s;
    };
    const GLuint vs = compile(VERTEX_SHADER, kVertex);
    const GLuint frag = compile(FRAGMENT_SHADER, fs.c_str());
    if (vs == 0 || frag == 0) return { nullptr, false };
    Program p;
    p.id = gl_.CreateProgram();
    gl_.AttachShader(p.id, vs);
    gl_.AttachShader(p.id, frag);
    gl_.BindAttribLocation(p.id, 0, "a_pos");
    gl_.BindAttribLocation(p.id, 1, "a_depth");
    gl_.BindAttribLocation(p.id, 2, "a_color");
    gl_.BindAttribLocation(p.id, 3, "a_stq");
    gl_.BindAttribLocation(p.id, 4, "a_rect");
    gl_.LinkProgram(p.id);
    gl_.DeleteShader(vs);
    gl_.DeleteShader(frag);
    GLint linked = 0;
    gl_.GetProgramiv(p.id, LINK_STATUS, &linked);
    if (!linked) {
        char log[1024] = {};
        gl_.GetProgramInfoLog(p.id, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[hard] program failed: %s\n", log);
        std::fflush(stderr);
        return { nullptr, false };
    }
    p.size = gl_.GetUniformLocation(p.id, "u_size");
    p.tex0 = gl_.GetUniformLocation(p.id, "u_tex0");
    p.tex1 = gl_.GetUniformLocation(p.id, "u_tex1");
    p.map0 = gl_.GetUniformLocation(p.id, "u_map0");
    p.map1 = gl_.GetUniformLocation(p.id, "u_map1");
    p.prim = gl_.GetUniformLocation(p.id, "u_prim");
    p.env = gl_.GetUniformLocation(p.id, "u_env");
    p.blend = gl_.GetUniformLocation(p.id, "u_blend");
    p.fog = gl_.GetUniformLocation(p.id, "u_fog");
    p.fill = gl_.GetUniformLocation(p.id, "u_fill");
    p.persp = gl_.GetUniformLocation(p.id, "u_persp");
    p.scale = gl_.GetUniformLocation(p.id, "u_scale");
    p.pad = gl_.GetUniformLocation(p.id, "u_pad");
    gl_.UseProgram(p.id);
    gl_.Uniform1i(p.tex0, 0);
    gl_.Uniform1i(p.tex1, 1);
    applied_valid_ = false;
    ++stats_.programs;
    return { &programs_.emplace(fs, p).first->second, blending };
}

// --- textures ------------------------------------------------------------------

HardRdp::Texture* HardRdp::texture_for_tile(uint32_t tile_index) {
    tile_index &= 7u;
    if (tile_texture_valid_[tile_index]) {
        if (Texture* t = tile_texture_[tile_index]) t->last_frame = frames_;
        return tile_texture_[tile_index];
    }
    tile_texture_valid_[tile_index] = true;
    tile_texture_[tile_index] = nullptr;
    const front::TileDesc& tile = tm_.tiles[tile_index];
    if (!tile.used || !tm_.image.valid) return nullptr;
    const uint32_t tlut = front::tlut_mode(other_mode_h_);
    const uint32_t cycle = other_mode_h_ & (3u << CycleShift);
    const bool bilinear = ((other_mode_h_ >> 12) & 3u) >= 2u && cycle != CycleCopy;

    // Addressing as the RDP (and soft) does it: a clamped axis stops at the
    // tile's edge; a masked one repeats (or mirrors) every 2^mask texels.
    const uint32_t width = front::Tmem::width(tile), height = front::Tmem::height(tile);
    auto axis = [](uint32_t cm, uint32_t mask, uint32_t size, uint32_t& dim, GLint& mode) {
        const uint32_t range = 1u << (mask & 0xFu);
        const bool clamp = (cm & 2u) != 0u || mask == 0u;
        if (clamp) {
            dim = size;
            mode = CLAMP_TO_EDGE;
        } else {
            dim = range;
            mode = (cm & 1u) != 0u ? MIRRORED_REPEAT : REPEAT;
        }
        dim = std::clamp<uint32_t>(dim, 1u, 1024u);
    };
    uint32_t dim_s = 1, dim_t = 1;
    GLint mode_s = CLAMP_TO_EDGE, mode_t = CLAMP_TO_EDGE;
    axis(tile.cm_s, tile.mask_s, width, dim_s, mode_s);
    axis(tile.cm_t, tile.mask_t, height, dim_t, mode_t);

    uint64_t key = tm_.fingerprint(tile, dim_s, dim_t, tlut);
    key ^= (uint64_t(mode_s) << 40) ^ (uint64_t(mode_t) << 20) ^ (bilinear ? 0x5A5A5A5A5A5A5A5Aull : 0ull);

    auto it = textures_.find(key);
    if (it == textures_.end()) {
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<uint32_t> texels(size_t(dim_s) * dim_t);
        for (uint32_t t = 0; t < dim_t; ++t) {
            for (uint32_t s = 0; s < dim_s; ++s) texels[size_t(t) * dim_s + s] = tm_.texel(tile, s, t, tlut);
        }
        Texture tex;
        gl_.GenTextures(1, &tex.id);
        gl_.BindTexture(TEXTURE_2D, tex.id);
        gl_.PixelStorei(UNPACK_ALIGNMENT, 4);
        gl_.TexImage2D(TEXTURE_2D, 0, RGBA8, dim_s, dim_t, 0, RGBA, UNSIGNED_BYTE, texels.data());
        gl_.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, bilinear ? LINEAR : NEAREST);
        gl_.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, bilinear ? LINEAR : NEAREST);
        gl_.TexParameteri(TEXTURE_2D, TEXTURE_WRAP_S, mode_s);
        gl_.TexParameteri(TEXTURE_2D, TEXTURE_WRAP_T, mode_t);
        applied_valid_ = false;
        ++stats_.textures_decoded;
        stats_.texture_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        it = textures_.emplace(key, tex).first;

        if (textures_.size() > 4096) {
            invalidate_tiles();   // entries are about to move
            // Forget what the last few frames did not use.
            for (auto e = textures_.begin(); e != textures_.end();) {
                if (e->first != key && e->second.last_frame + 4 < frames_) {
                    gl_.DeleteTextures(1, &e->second.id);
                    e = textures_.erase(e);
                } else {
                    ++e;
                }
            }
            it = textures_.find(key);
        }
    }
    Texture& tex = it->second;
    tex.last_frame = frames_;

    // Texel coordinate -> GL coordinate: the tile's shift, less its origin, over
    // the texture's size. Bilinear: the RDP blends texel floor(s) toward the next
    // by frac(s); GL blends around texel centres, hence the half.
    auto shift_scale = [](uint32_t shift) {
        if (shift == 0) return 1.0f;
        return shift <= 10 ? 1.0f / static_cast<float>(1u << shift) : static_cast<float>(1u << (16 - shift));
    };
    // Point sampling: a coordinate that is exactly a whole texel (a rectangle
    // stepping by 1) must not round down through the float divide, so it is
    // nudged by less than the RDP's 1/256-texel precision.
    const float half = bilinear ? 0.5f : 1.0f / 512.0f;
    tex.map[0] = shift_scale(tile.shift_s) / static_cast<float>(dim_s);
    tex.map[1] = (half - static_cast<float>(tile.uls) / 4.0f) / static_cast<float>(dim_s);
    tex.map[2] = shift_scale(tile.shift_t) / static_cast<float>(dim_t);
    tex.map[3] = (half - static_cast<float>(tile.ult) / 4.0f) / static_cast<float>(dim_t);
    tile_texture_[tile_index] = &tex;
    return &tex;
}

// --- targets -------------------------------------------------------------------

HardRdp::Target* HardRdp::current_target() {
    if (zimg_address_ != 0 && cimg_address_ == zimg_address_) return nullptr;
    auto it = targets_.find(cimg_address_);
    if (it != targets_.end() && it->second.width == cimg_width_) return &it->second;
    if (it != targets_.end()) {
        if (bound_ == &it->second) bound_ = nullptr;
        if (shown_ == &it->second) shown_ = nullptr;
        gl_.DeleteFramebuffers(1, &it->second.fbo);
        gl_.DeleteTextures(1, &it->second.color);
        targets_.erase(it);
    }

    Target t;
    t.address = cimg_address_;
    t.width = cimg_width_;
    t.siz = cimg_siz_;
    // The RDP is never told an image's height; the scissor set for it is the
    // best witness, then 4:3.
    const uint32_t scissor_h = (scissor_[3] + 3u) >> 2;
    t.height = (scissor_h > 0 && scissor_h <= 1024 && (scissor_[2] >> 2) <= t.width + 1) ? scissor_h : t.width * 3 / 4;
    t.pad = static_cast<uint32_t>(std::lround(static_cast<float>(t.width) * (extension_ - 1.0f) / 2.0f));
    t.ext_width = t.width + 2u * t.pad;
    const GLsizei w = static_cast<GLsizei>(t.ext_width) * scale_, h = static_cast<GLsizei>(t.height) * scale_;

    gl_.GenTextures(1, &t.color);
    gl_.BindTexture(TEXTURE_2D, t.color);
    gl_.TexImage2D(TEXTURE_2D, 0, RGBA8, w, h, 0, RGBA, UNSIGNED_BYTE, nullptr);
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, LINEAR);
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, LINEAR);
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_WRAP_S, CLAMP_TO_EDGE);
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_WRAP_T, CLAMP_TO_EDGE);

    const uint64_t key = (uint64_t(w) << 32) | uint64_t(h);
    auto d = depth_buffers_.find(key);
    if (d == depth_buffers_.end()) {
        GLuint rb = 0;
        gl_.GenRenderbuffers(1, &rb);
        gl_.BindRenderbuffer(RENDERBUFFER, rb);
        gl_.RenderbufferStorage(RENDERBUFFER, DEPTH_COMPONENT24, w, h);
        d = depth_buffers_.emplace(key, rb).first;
    }
    t.depth = d->second;

    gl_.GenFramebuffers(1, &t.fbo);
    gl_.BindFramebuffer(FRAMEBUFFER, t.fbo);
    gl_.FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, t.color, 0);
    gl_.FramebufferRenderbuffer(FRAMEBUFFER, DEPTH_ATTACHMENT, RENDERBUFFER, t.depth);
    if (gl_.CheckFramebufferStatus(FRAMEBUFFER) != FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[hard] target 0x%06X %ux%u incomplete\n", t.address, t.width, t.height);
    }
    gl_.Disable(SCISSOR_TEST);
    gl_.ClearColor(0, 0, 0, 1);
    gl_.clear_depth(1.0f);
    gl_.DepthMask(TRUE);
    gl_.Clear(COLOR_BUFFER_BIT | DEPTH_BUFFER_BIT);
    gl_.Enable(SCISSOR_TEST);
    applied_valid_ = false;
    scissor_set_[2] = -1;
    bound_ = nullptr;

    ++stats_.targets_created;
    std::fprintf(stderr, "[hard] new target 0x%06X %ux%u (%dx%d)\n", t.address, t.width, t.height, w, h);
    std::fflush(stderr);
    Target& ref = targets_.emplace(t.address, t).first->second;
    // Whatever the CPU left there is the picture the RDP draws over.
    import_rdram(ref);
    return &ref;
}

void HardRdp::decode_rdram(uint32_t address, uint32_t width, uint32_t height, uint32_t siz,
                           std::vector<uint32_t>& rgba) const {
    // Rows bottom-up, as GL stores them.
    rgba.assign(size_t(width) * height, 0xFF000000u);
    const uint32_t bpp = siz == 3 ? 4u : 2u;
    for (uint32_t y = 0; y < height; ++y) {
        uint32_t* row = rgba.data() + size_t(height - 1 - y) * width;
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t a = address + (y * width + x) * bpp;
            if (a + bpp > front::RdramSize) continue;
            uint32_t r, g, b;
            if (bpp == 2) {
                uint16_t v;
                std::memcpy(&v, rdram_ + (a ^ 2u), 2);
                r = expand5(v >> 11);
                g = expand5(v >> 6);
                b = expand5(v >> 1);
            } else {
                uint32_t v;
                std::memcpy(&v, rdram_ + a, 4);
                r = v >> 24;
                g = (v >> 16) & 0xFFu;
                b = (v >> 8) & 0xFFu;
            }
            row[x] = r | (g << 8) | (b << 16) | 0xFF000000u;   // RGBA bytes in memory order
        }
    }
}

void HardRdp::import_rdram(Target& t) {
    std::vector<uint32_t> pixels;
    decode_rdram(t.address, t.width, t.height, t.siz, pixels);
    GLuint tex = 0, fbo = 0;
    gl_.GenTextures(1, &tex);
    gl_.BindTexture(TEXTURE_2D, tex);
    gl_.PixelStorei(UNPACK_ALIGNMENT, 4);
    gl_.TexImage2D(TEXTURE_2D, 0, RGBA8, t.width, t.height, 0, RGBA, UNSIGNED_BYTE, pixels.data());
    gl_.GenFramebuffers(1, &fbo);
    gl_.BindFramebuffer(READ_FRAMEBUFFER, fbo);
    gl_.FramebufferTexture2D(READ_FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, tex, 0);
    gl_.BindFramebuffer(DRAW_FRAMEBUFFER, t.fbo);
    gl_.Disable(SCISSOR_TEST);
    gl_.BlitFramebuffer(0, 0, t.width, t.height, t.pad * scale_, 0, (t.pad + t.width) * scale_, t.height * scale_,
                        COLOR_BUFFER_BIT, NEAREST);
    gl_.Enable(SCISSOR_TEST);
    gl_.BindFramebuffer(FRAMEBUFFER, 0);
    gl_.DeleteFramebuffers(1, &fbo);
    gl_.DeleteTextures(1, &tex);
    applied_valid_ = false;
    scissor_set_[2] = -1;
    bound_ = nullptr;
}

// --- state and batching --------------------------------------------------------

bool HardRdp::bind_target_state() {
    Target* t = current_target();
    if (t != bound_) {
        flush();
        bound_ = t;
        if (t == nullptr) return false;
        gl_.BindFramebuffer(FRAMEBUFFER, t->fbo);
        gl_.Viewport(0, 0, t->ext_width * scale_, t->height * scale_);
        scissor_set_[2] = -1;
        applied_valid_ = false;   // u_size follows the target
    }
    if (t == nullptr) return false;
    if (depth_clear_pending_) {
        flush();
        gl_.Disable(SCISSOR_TEST);
        gl_.DepthMask(TRUE);
        gl_.clear_depth(1.0f);
        gl_.Clear(DEPTH_BUFFER_BIT);
        gl_.Enable(SCISSOR_TEST);
        applied_valid_ = false;
        depth_clear_pending_ = false;
    }
    // Scissor, 10.2 native pixels -> target pixels, y up.
    const int s = scale_;
    int sx0 = static_cast<int>(scissor_[0] >> 2), sx1 = static_cast<int>((scissor_[2] + 3u) >> 2);
    if (t->pad > 0 && sx0 <= static_cast<int>(t->width) / 10 && sx1 >= static_cast<int>(t->width) * 9 / 10) {
        sx0 = 0;
        sx1 = static_cast<int>(t->ext_width);
    } else {
        sx0 += static_cast<int>(t->pad);
        sx1 += static_cast<int>(t->pad);
    }
    const int x0 = sx0 * s;
    const int x1 = sx1 * s;
    const int y0 = static_cast<int>(scissor_[1] >> 2);
    const int y1 = static_cast<int>((scissor_[3] + 3u) >> 2);
    const int gy = (static_cast<int>(t->height) - y1) * s;
    const int gh = (y1 - y0) * s;
    if (x0 != scissor_set_[0] || gy != scissor_set_[1] || x1 - x0 != scissor_set_[2] || gh != scissor_set_[3]) {
        flush();
        gl_.Scissor(x0, gy, std::max(0, x1 - x0), std::max(0, gh));
        scissor_set_[0] = x0;
        scissor_set_[1] = gy;
        scissor_set_[2] = x1 - x0;
        scissor_set_[3] = gh;
    }
    return true;
}

void HardRdp::begin_primitive(bool textured, uint32_t tile, int depth_key, float persp) {
    DrawState s;
    bool blending = false;
    s.program = program_for_state(textured, blending);
    s.blending = blending ? 1 : 0;
    const uint32_t cycle = other_mode_h_ & (3u << CycleShift);
    if (textured) {
        if (Texture* t0 = texture_for_tile(tile)) {
            s.tex[0] = t0->id;
            std::memcpy(s.map[0], t0->map, sizeof(s.map[0]));
        }
        // TEXEL1 is the next tile; only a two-cycle combiner reads it.
        if (cycle == Cycle2) {
            if (Texture* t1 = texture_for_tile((tile + 1u) & 7u)) {
                s.tex[1] = t1->id;
                std::memcpy(s.map[1], t1->map, sizeof(s.map[1]));
            }
        }
    }
    std::memcpy(s.prim, prim_color_, sizeof(s.prim));
    std::memcpy(s.env, env_color_, sizeof(s.env));
    std::memcpy(s.blend, blend_color_, sizeof(s.blend));
    std::memcpy(s.fog, fog_color_, sizeof(s.fog));
    if (cimg_siz_ == 3) {
        set_rgba(s.fill, fill_color_);
    } else {
        const uint32_t c = fill_color_ & 0xFFFFu;
        s.fill[0] = static_cast<float>(expand5(c >> 11)) / 255.0f;
        s.fill[1] = static_cast<float>(expand5(c >> 6)) / 255.0f;
        s.fill[2] = static_cast<float>(expand5(c >> 1)) / 255.0f;
    }
    s.fill[3] = 1.0f;
    s.persp = persp;
    s.depth = depth_key;
    if (!(s == batch_state_)) {
        flush();
        batch_state_ = s;
    }
}

void HardRdp::apply_draw_state(const DrawState& s) {
    const bool first = !applied_valid_;
    if (first || s.program != applied_.program) gl_.UseProgram(s.program->id);
    Program& p = *s.program;

    // Uniforms live in the program: send only what differs from what it holds.
    const bool primed = p.primed;
    auto set4 = [&](GLint loc, float* held, const float* v) {
        if (primed && std::memcmp(held, v, 4 * sizeof(float)) == 0) return;
        std::memcpy(held, v, 4 * sizeof(float));
        gl_.Uniform4f(loc, v[0], v[1], v[2], v[3]);
    };
    auto set1 = [&](GLint loc, float& held, float v) {
        if (primed && held == v) return;
        held = v;
        gl_.Uniform1f(loc, v);
    };
    if (bound_ != nullptr) {
        const float w = static_cast<float>(bound_->ext_width), h = static_cast<float>(bound_->height);
        if (!primed || p.held_size[0] != w || p.held_size[1] != h) {
            p.held_size[0] = w;
            p.held_size[1] = h;
            gl_.Uniform2f(p.size, w, h);
        }
        set1(p.pad, p.held_pad, static_cast<float>(bound_->pad));
    }
    set4(p.map0, p.held_map[0], s.map[0]);
    set4(p.map1, p.held_map[1], s.map[1]);
    set4(p.prim, p.held_prim, s.prim);
    set4(p.env, p.held_env, s.env);
    set4(p.blend, p.held_blend, s.blend);
    set4(p.fog, p.held_fog, s.fog);
    set4(p.fill, p.held_fill, s.fill);
    set1(p.persp, p.held_persp, s.persp);
    set1(p.scale, p.held_scale, static_cast<float>(scale_));
    p.primed = true;

    // Textures: rebind only what changed (other code binding textures clears
    // applied_valid_, which forgets what the units hold).
    if (first) bound_tex_[0] = bound_tex_[1] = ~0u;
    const GLuint t0 = s.tex[0] != 0 ? s.tex[0] : white_, t1 = s.tex[1] != 0 ? s.tex[1] : white_;
    if (t1 != bound_tex_[1]) {
        gl_.ActiveTexture(TEXTURE0 + 1);
        gl_.BindTexture(TEXTURE_2D, t1);
        gl_.ActiveTexture(TEXTURE0);
        bound_tex_[1] = t1;
    }
    if (t0 != bound_tex_[0]) {
        gl_.ActiveTexture(TEXTURE0);
        gl_.BindTexture(TEXTURE_2D, t0);
        bound_tex_[0] = t0;
    }
    if (first || s.depth != applied_.depth) {
        if (s.depth & 1) gl_.Enable(DEPTH_TEST);
        else gl_.Disable(DEPTH_TEST);
        gl_.DepthMask((s.depth & 2) ? TRUE : FALSE);
    }
    if (first || s.blending != applied_.blending) {
        if (s.blending) {
            gl_.Enable(BLEND);
            gl_.BlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA);
        } else {
            gl_.Disable(BLEND);
        }
    }
    applied_ = s;
    applied_valid_ = true;
}

void HardRdp::push_vertex(float x, float y, float z, const float* rgba, float s, float t, float q,
                          const float* rect) {
    static const float none[4] = { 0, 0, 0, 0 };
    if (rect == nullptr) rect = none;
    batch_.insert(batch_.end(),
                  { x, y, z, rgba[0], rgba[1], rgba[2], rgba[3], s, t, q, rect[0], rect[1], rect[2], rect[3] });
}

void HardRdp::flush() {
    if (batch_.empty() || bound_ == nullptr || batch_state_.program == nullptr) {
        batch_.clear();
        return;
    }
    struct Timer {
        double& out;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~Timer() { out += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
    } timer{ stats_.draw_ms };
    apply_draw_state(batch_state_);
    gl_.BindVertexArray(vao_);
    gl_.BindBuffer(ARRAY_BUFFER, vbo_);
    // One upload per draw: measured cheapest on both desktop GL and the Mali
    // (a sub-data upload into a busy buffer made the Mali copy all of it; a
    // mapped stream buffer cost more per draw on desktop and gained nothing there).
    gl_.BufferData(ARRAY_BUFFER, static_cast<GLsizeiptr>(batch_.size() * sizeof(float)), batch_.data(), STREAM_DRAW);
    gl_.DrawArrays(TRIANGLES, 0, static_cast<GLsizei>(batch_.size() / VertexFloats));
    bound_->dirty = true;
    ++stats_.draws;
    batch_.clear();
}

// --- state commands ------------------------------------------------------------

void HardRdp::set_color_image(uint32_t fmt, uint32_t siz, uint32_t width, uint32_t address) {
    (void)fmt;
    cimg_address_ = address & 0x03FFFFFFu;
    cimg_width_ = width;
    cimg_siz_ = siz;
}

void HardRdp::set_depth_image(uint32_t address) { zimg_address_ = address & 0x03FFFFFFu; }

void HardRdp::set_texture_image(uint32_t fmt, uint32_t siz, uint32_t width, uint32_t address) {
    tm_.set_texture_image(fmt, siz, width, address);
}
void HardRdp::set_tile(uint32_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint32_t tile, uint32_t palette,
                       uint32_t cm_t, uint32_t mask_t, uint32_t shift_t, uint32_t cm_s, uint32_t mask_s,
                       uint32_t shift_s) {
    invalidate_tiles();
    tm_.set_tile(fmt, siz, line, tmem, tile, palette, cm_t, mask_t, shift_t, cm_s, mask_s, shift_s);
}
void HardRdp::set_tile_size(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    invalidate_tiles();
    tm_.set_tile_size(tile, uls, ult, lrs, lrt);
}
void HardRdp::load_block(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) {
    sync_texture_source();
    invalidate_tiles();
    tm_.load_block(tile, uls, ult, lrs, dxt);
}
void HardRdp::load_tile(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    sync_texture_source();
    invalidate_tiles();
    tm_.load_tile(tile, uls, ult, lrs, lrt);
}
void HardRdp::load_tlut(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    sync_texture_source();
    invalidate_tiles();
    tm_.load_tlut(tile, uls, ult, lrs, lrt);
}

void HardRdp::sync_texture_source() {
    if (!tm_.image.valid) return;
    const uint32_t address = tm_.image.address;
    for (auto& [start, t] : targets_) {
        const uint32_t bytes = t.width * t.height * (t.siz == 3 ? 4u : 2u);
        if (t.dirty && address >= start && address < start + bytes) {
            flush();   // the target's pending draws belong in what is read
            write_back(t);
        }
    }
}

void HardRdp::end_task() {
    flush();
    if (!copy_back_every_task_) return;
    for (auto& [start, t] : targets_) {
        if (t.dirty) write_back(t);
    }
}

void HardRdp::write_back(Target& t) {
    // Down to native size on the GPU, then into RDRAM in the image's own format.
    const GLsizei w = static_cast<GLsizei>(t.width), h = static_cast<GLsizei>(t.height);
    GLuint tex = 0, fbo = 0;
    gl_.GenTextures(1, &tex);
    gl_.BindTexture(TEXTURE_2D, tex);
    gl_.TexImage2D(TEXTURE_2D, 0, RGBA8, w, h, 0, RGBA, UNSIGNED_BYTE, nullptr);
    gl_.GenFramebuffers(1, &fbo);
    gl_.BindFramebuffer(DRAW_FRAMEBUFFER, fbo);
    gl_.FramebufferTexture2D(DRAW_FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, tex, 0);
    gl_.BindFramebuffer(READ_FRAMEBUFFER, t.fbo);
    gl_.Disable(SCISSOR_TEST);
    const GLint src_x = static_cast<GLint>(t.pad) * scale_;
    gl_.BlitFramebuffer(src_x, 0, src_x + w * scale_, h * scale_, 0, 0, w, h, COLOR_BUFFER_BIT, NEAREST);
    gl_.Enable(SCISSOR_TEST);
    std::vector<uint32_t> rgba(size_t(w) * h);
    gl_.BindFramebuffer(READ_FRAMEBUFFER, fbo);
    gl_.PixelStorei(PACK_ALIGNMENT, 4);
    gl_.ReadPixels(0, 0, w, h, RGBA, UNSIGNED_BYTE, rgba.data());
    gl_.BindFramebuffer(FRAMEBUFFER, 0);
    gl_.DeleteFramebuffers(1, &fbo);
    gl_.DeleteTextures(1, &tex);

    const uint32_t bpp = t.siz == 3 ? 4u : 2u;
    for (GLsizei y = 0; y < h; ++y) {
        const uint32_t* row = rgba.data() + size_t(h - 1 - y) * w;   // GL rows run bottom-up
        for (GLsizei x = 0; x < w; ++x) {
            const uint32_t a = t.address + (uint32_t(y) * t.width + uint32_t(x)) * bpp;
            if (a + bpp > front::RdramSize) continue;
            const uint32_t p = row[x];   // bytes R G B A
            const uint32_t r = p & 0xFFu, g = (p >> 8) & 0xFFu, b = (p >> 16) & 0xFFu;
            if (bpp == 2) {
                const uint16_t v = static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1u);
                std::memcpy(rdram_ + (a ^ 2u), &v, 2);
            } else {
                const uint32_t v = (r << 24) | (g << 16) | (b << 8) | 0xFFu;
                std::memcpy(rdram_ + a, &v, 4);
            }
        }
    }
    t.dirty = false;
    ++stats_.write_backs;
    applied_valid_ = false;
    scissor_set_[2] = -1;
    bound_ = nullptr;
}

void HardRdp::set_combine(uint32_t w0, uint32_t w1) {
    combine_l_ = w0 & 0x00FFFFFFu;
    combine_h_ = w1;
}

void HardRdp::set_other_mode_h(uint32_t shift, uint32_t length, uint32_t data) {
    const uint32_t mask = ((1u << length) - 1u) << shift;
    const uint32_t before = other_mode_h_;
    other_mode_h_ = (other_mode_h_ & ~mask) | (data & mask);
    // The TLUT mode, the filter and the cycle type change how tiles decode.
    if ((before ^ other_mode_h_) & ((3u << 14) | (3u << 12) | (3u << CycleShift))) invalidate_tiles();
}

void HardRdp::set_other_mode_l(uint32_t shift, uint32_t length, uint32_t data) {
    const uint32_t mask = (length >= 32 ? 0xFFFFFFFFu : ((1u << length) - 1u)) << shift;
    other_mode_l_ = (other_mode_l_ & ~mask) | (data & mask);
}

void HardRdp::set_env_color(uint32_t rgba) { set_rgba(env_color_, rgba); }
void HardRdp::set_prim_color(uint32_t rgba) { set_rgba(prim_color_, rgba); }
void HardRdp::set_blend_color(uint32_t rgba) { set_rgba(blend_color_, rgba); }
void HardRdp::set_fog_color(uint32_t rgba) { set_rgba(fog_color_, rgba); }
void HardRdp::set_fill_color(uint32_t rgba) { fill_color_ = rgba; }
void HardRdp::set_prim_depth(uint32_t) {}

void HardRdp::set_scissor(uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    scissor_[0] = ulx;
    scissor_[1] = uly;
    scissor_[2] = lrx;
    scissor_[3] = lry;
}

void HardRdp::set_geometry_mode(uint32_t) {}

// --- primitives ----------------------------------------------------------------

void HardRdp::fill_rect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    ++stats_.fill_rects;
    if (zimg_address_ != 0 && cimg_address_ == zimg_address_) {
        // Filling the depth image clears it; the colour targets of its size share it.
        flush();
        depth_clear_pending_ = true;
        return;
    }
    const uint32_t cycle = other_mode_h_ & (3u << CycleShift);
    uint32_t right = static_cast<uint32_t>(lrx), bottom = static_cast<uint32_t>(lry);
    if (cycle == CycleFill || cycle == CycleCopy) {
        // Fill and copy rectangles include their lower-right edge.
        right |= 3u;
        bottom |= 3u;
    }
    float x0 = static_cast<float>(static_cast<uint32_t>(ulx) >> 2);
    const float y0 = static_cast<float>(static_cast<uint32_t>(uly) >> 2);
    float x1 = static_cast<float>((right + 3u) >> 2);
    const float y1 = static_cast<float>((bottom + 3u) >> 2);
    if (x1 <= x0 || y1 <= y0) return;
    if (!bind_target_state()) return;
    // Widescreen: a fill across the image (a clear, a backdrop) spans the target.
    const float width = static_cast<float>(bound_->width), pad = static_cast<float>(bound_->pad);
    if (pad > 0.0f && x0 <= width * 0.1f && x1 >= width * 0.9f) {
        x0 = -pad;
        x1 = width + pad;
    }

    // A fill rectangle writes the fill colour, as soft draws it, whatever the cycle.
    const uint32_t saved = other_mode_h_;
    other_mode_h_ = (other_mode_h_ & ~(3u << CycleShift)) | CycleFill;
    begin_primitive(false, 0, 0, 1.0f);
    other_mode_h_ = saved;

    const float white[4] = { 1, 1, 1, 1 };
    push_vertex(x0, y0, 0, white, 0, 0, 1);
    push_vertex(x0, y1, 0, white, 0, 0, 1);
    push_vertex(x1, y0, 0, white, 0, 0, 1);
    push_vertex(x0, y1, 0, white, 0, 0, 1);
    push_vertex(x1, y1, 0, white, 0, 0, 1);
    push_vertex(x1, y0, 0, white, 0, 0, 1);
}

void HardRdp::tex_rect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint32_t tile, int32_t uls, int32_t ult,
                       int32_t dsdx, int32_t dtdy, bool flip) {
    ++stats_.tex_rects;
    static const char* rect_log = std::getenv("WETTER_HARD_RECT_LOG");
    if (rect_log != nullptr && frames_ == std::strtoull(rect_log, nullptr, 10)) {
        const front::TileDesc& d = tm_.tiles[tile & 7u];
        std::fprintf(stderr, "[hard] texrect (%d,%d)-(%d,%d) tile %u st %d,%d d %d,%d flip %d | tile %ux%u ul %u,%u cm %u,%u mask %u,%u shift %u,%u\n",
                     ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, flip ? 1 : 0, front::Tmem::width(d),
                     front::Tmem::height(d), d.uls, d.ult, d.cm_s, d.cm_t, d.mask_s, d.mask_t, d.shift_s, d.shift_t);
    }
    if (lrx <= ulx || lry <= uly) return;
    const float x0 = static_cast<float>(static_cast<uint32_t>(ulx) >> 2);
    const float y0 = static_cast<float>(static_cast<uint32_t>(uly) >> 2);
    const float x1 = static_cast<float>((static_cast<uint32_t>(lrx) + 3u) >> 2);
    const float y1 = static_cast<float>((static_cast<uint32_t>(lry) + 3u) >> 2);
    if (x1 <= x0 || y1 <= y0) return;
    if (!bind_target_state()) return;
    begin_primitive(true, tile, 0, 1.0f);

    // s/t = origin + pixel * step (S10.5, S5.10), pixel counted from the
    // rectangle's corner; the shader works the pixel out per fragment.
    const float origin_s = static_cast<float>(uls) / 32.0f, origin_t = static_cast<float>(ult) / 32.0f;
    const float step[4] = { x0 + static_cast<float>(bound_->pad), y0, static_cast<float>(dsdx) / 1024.0f,
                            static_cast<float>(dtdy) / 1024.0f };
    const float q = flip ? -2.0f : -1.0f;
    const float white[4] = { 1, 1, 1, 1 };
    push_vertex(x0, y0, 0, white, origin_s, origin_t, q, step);
    push_vertex(x0, y1, 0, white, origin_s, origin_t, q, step);
    push_vertex(x1, y0, 0, white, origin_s, origin_t, q, step);
    push_vertex(x0, y1, 0, white, origin_s, origin_t, q, step);
    push_vertex(x1, y1, 0, white, origin_s, origin_t, q, step);
    push_vertex(x1, y0, 0, white, origin_s, origin_t, q, step);
}

void HardRdp::triangle(const front::ScreenVertex& a, const front::ScreenVertex& b, const front::ScreenVertex& c,
                       bool textured, uint32_t tile) {
    ++stats_.triangles;
    if (!bind_target_state()) return;
    const bool test = zimg_address_ != 0 && (other_mode_l_ & ZCompare) != 0;
    const int depth_key = (test ? 1 : 0) | (test && (other_mode_l_ & ZUpdate) != 0 ? 2 : 0);
    // Without G_TP_PERSP the RDP skips the divide; soft halves s/t to match.
    const float persp = (other_mode_h_ & (1u << 19)) != 0u ? 1.0f : 0.5f;
    begin_primitive(textured, tile, depth_key, persp);
    for (const front::ScreenVertex* v : { &a, &b, &c }) {
        const float rgba[4] = { v->r, v->g, v->b, v->a };
        const float q = v->w != 0.0f ? v->w : 1e-6f;
        push_vertex(v->x, v->y, std::clamp(v->z, 0.0f, 1.0f), rgba, v->s * q, v->t * q, q);
    }
}

// --- the picture ---------------------------------------------------------------

HardRdp::Target* HardRdp::find_shown(uint32_t origin) {
    for (auto& [address, t] : targets_) {
        const uint32_t bytes = t.width * t.height * (t.siz == 3 ? 4u : 2u);
        if (origin >= address && origin < address + bytes) return &t;
    }
    return nullptr;
}

void HardRdp::compose(uint32_t vi_origin, uint32_t vi_width, uint32_t vi_status, uint32_t vi_v_start,
                      uint32_t vi_y_scale) {
    flush();
    ++frames_;
    const uint32_t origin = vi_origin & 0x00FFFFFFu;
    shown_ = find_shown(origin);
    shown_cpu_ = false;
    if (shown_ != nullptr) {
        // The VI starts at its origin, which may sit lines into the image, and
        // scans out (v_end - v_start) / 2 * y_scale lines; 240 without a figure.
        const uint32_t row_bytes = shown_->width * (shown_->siz == 3 ? 4u : 2u);
        shown_y0_ = row_bytes != 0 ? (origin - shown_->address) / row_bytes : 0u;
        uint32_t lines = 240;
        const uint32_t v_start = (vi_v_start >> 16) & 0x3FFu, v_end = vi_v_start & 0x3FFu;
        const uint32_t y_scale = vi_y_scale & 0xFFFu;
        if (v_end > v_start && y_scale != 0u) {
            const uint32_t n = (((v_end - v_start) / 2u) * y_scale) >> 10;
            if (n > 0 && n < lines) lines = n;
        }
        shown_y0_ = std::min(shown_y0_, shown_->height - 1u);
        shown_lines_ = std::min(lines, shown_->height - shown_y0_);
        static const bool vi_log = std::getenv("WETTER_VI_LOG") != nullptr;
        static uint32_t last = 0xFFFFFFFFu;
        if (vi_log && (origin ^ (shown_y0_ << 24) ^ shown_lines_) != last) {
            last = origin ^ (shown_y0_ << 24) ^ shown_lines_;
            std::fprintf(stderr, "[hard] VI origin 0x%06X -> target 0x%06X line %u, %u lines (v_start %08X y_scale %08X)\n",
                         origin, shown_->address, shown_y0_, shown_lines_, vi_v_start, vi_y_scale);
        }
    }
    if (shown_ == nullptr && (vi_status & 3u) >= 2u && origin != 0 && vi_width != 0) {
        // Nothing the RDP drew: show what the CPU left in RDRAM.
        const uint32_t w = vi_width, h = std::clamp<uint32_t>(vi_width * 3 / 4, 1, 576);
        const uint32_t siz = (vi_status & 3u) == 3u ? 3u : 2u;
        std::vector<uint32_t> pixels;
        decode_rdram(origin, w, h, siz, pixels);
        if (cpu_picture_.width != w || cpu_picture_.height != h) {
            if (cpu_picture_.fbo != 0) {
                gl_.DeleteFramebuffers(1, &cpu_picture_.fbo);
                gl_.DeleteTextures(1, &cpu_picture_.color);
            }
            cpu_picture_ = Target{};
            cpu_picture_.width = w;
            cpu_picture_.height = h;
            gl_.GenTextures(1, &cpu_picture_.color);
            gl_.BindTexture(TEXTURE_2D, cpu_picture_.color);
            gl_.TexImage2D(TEXTURE_2D, 0, RGBA8, w, h, 0, RGBA, UNSIGNED_BYTE, nullptr);
            gl_.GenFramebuffers(1, &cpu_picture_.fbo);
            gl_.BindFramebuffer(FRAMEBUFFER, cpu_picture_.fbo);
            gl_.FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, cpu_picture_.color, 0);
            gl_.BindFramebuffer(FRAMEBUFFER, 0);
        }
        gl_.BindTexture(TEXTURE_2D, cpu_picture_.color);
        gl_.PixelStorei(UNPACK_ALIGNMENT, 4);
        gl_.TexSubImage2D(TEXTURE_2D, 0, 0, 0, w, h, RGBA, UNSIGNED_BYTE, pixels.data());
        shown_ = &cpu_picture_;
        shown_cpu_ = true;
        shown_y0_ = 0;
        shown_lines_ = h;
    }
    applied_valid_ = false;
    bound_ = nullptr;   // the host's framebuffer is bound next
}

void HardRdp::present(int width, int height) {
    gl_.BindFramebuffer(FRAMEBUFFER, 0);
    gl_.Disable(SCISSOR_TEST);
    gl_.Viewport(0, 0, width, height);
    gl_.ClearColor(0, 0, 0, 1);
    gl_.Clear(COLOR_BUFFER_BIT);
    if (shown_ != nullptr) {
        const int scale = shown_cpu_ ? 1 : scale_;
        const uint32_t ext = shown_cpu_ ? shown_->width : shown_->ext_width;
        const int sw = static_cast<int>(ext) * scale;
        // Rows y0 .. y0 + lines, top-down; GL counts from the bottom.
        const int top = (static_cast<int>(shown_->height) - static_cast<int>(shown_y0_)) * scale;
        const int bottom = top - static_cast<int>(shown_lines_) * scale;
        // The VI shows the image at 4:3 whatever its line count; a widened
        // target is that much wider.
        const float aspect = 4.0f / 3.0f * static_cast<float>(ext) / static_cast<float>(shown_->width);
        int dw = width, dh = height;
        if (static_cast<float>(width) > static_cast<float>(height) * aspect) dw = static_cast<int>(std::lround(height * aspect));
        else dh = static_cast<int>(std::lround(width / aspect));
        const int dx = (width - dw) / 2, dy = (height - dh) / 2;
        gl_.BindFramebuffer(READ_FRAMEBUFFER, shown_->fbo);
        gl_.BindFramebuffer(DRAW_FRAMEBUFFER, 0);
        gl_.BlitFramebuffer(0, bottom, sw, top, dx, dy, dx + dw, dy + dh, COLOR_BUFFER_BIT, LINEAR);
        gl_.BindFramebuffer(FRAMEBUFFER, 0);
    }
    gl_.Enable(SCISSOR_TEST);
    scissor_set_[2] = -1;
    applied_valid_ = false;
    bound_ = nullptr;
}

bool HardRdp::read_shown(std::vector<uint32_t>& out, int& width, int& height) {
    if (shown_ == nullptr) return false;
    const int scale = shown_cpu_ ? 1 : scale_;
    width = static_cast<int>(shown_cpu_ ? shown_->width : shown_->ext_width) * scale;
    height = static_cast<int>(shown_lines_) * scale;
    const int bottom = (static_cast<int>(shown_->height) - static_cast<int>(shown_y0_ + shown_lines_)) * scale;
    std::vector<uint32_t> rgba(size_t(width) * height);
    gl_.BindFramebuffer(FRAMEBUFFER, shown_->fbo);
    gl_.PixelStorei(PACK_ALIGNMENT, 4);
    gl_.ReadPixels(0, bottom, width, height, RGBA, UNSIGNED_BYTE, rgba.data());
    gl_.BindFramebuffer(FRAMEBUFFER, 0);
    bound_ = nullptr;
    applied_valid_ = false;
    out.resize(rgba.size());
    for (int y = 0; y < height; ++y) {
        const uint32_t* src = rgba.data() + size_t(height - 1 - y) * width;
        uint32_t* dst = out.data() + size_t(y) * width;
        for (int x = 0; x < width; ++x) {
            const uint32_t p = src[x];   // bytes R G B A
            dst[x] = 0xFF000000u | ((p & 0xFFu) << 16) | (p & 0xFF00u) | ((p >> 16) & 0xFFu);
        }
    }
    return true;
}

}  // namespace wetter::hard
