// front end: the RSP. See rsp.h.

#include "rsp.h"

#include <cmath>
#include <cstdlib>
#include <cstring>


namespace wetter::front {
namespace {

uint8_t read_byte(const uint8_t* rdram, uint32_t address) {
    if (address >= RdramSize) return 0;
    return rdram[address ^ 3u];
}

uint32_t read_word(const uint8_t* rdram, uint32_t address) {
    return (static_cast<uint32_t>(read_byte(rdram, address)) << 24) |
           (static_cast<uint32_t>(read_byte(rdram, address + 1u)) << 16) |
           (static_cast<uint32_t>(read_byte(rdram, address + 2u)) << 8) |
           static_cast<uint32_t>(read_byte(rdram, address + 3u));
}

int16_t read_half(const uint8_t* rdram, uint32_t address) {
    return static_cast<int16_t>((static_cast<uint16_t>(read_byte(rdram, address)) << 8) |
                                read_byte(rdram, address + 1u));
}

float read_fixed(const uint8_t* rdram, uint32_t integer_address, uint32_t fraction_address) {
    const int16_t integer_part = read_half(rdram, integer_address);
    const uint16_t fraction_part =
        static_cast<uint16_t>(read_half(rdram, fraction_address));
    return static_cast<float>(integer_part) + static_cast<float>(fraction_part) / 65536.0f;
}

enum Plane { PlaneXMin = 0, PlaneXMax, PlaneYMin, PlaneYMax, PlaneZMin, PlaneZMax };
constexpr uint32_t PlaneCount = 6;

enum class ClipMode { Xy, Full, None };

ClipMode clip_mode() {
    static const ClipMode mode = [] {
        if (std::getenv("WETTER_NO_CLIP") != nullptr) return ClipMode::None;
        const char* value = std::getenv("WETTER_CLIP");
        if (value == nullptr) return ClipMode::Xy;
        if (std::strcmp(value, "full") == 0) return ClipMode::Full;
        if (std::strcmp(value, "none") == 0) return ClipMode::None;
        return ClipMode::Xy;
    }();
    return mode;
}

bool plane_used(ClipMode mode, uint32_t plane) {
    switch (mode) {
        case ClipMode::None: return false;
        case ClipMode::Full: return true;
        default: return plane != PlaneZMin && plane != PlaneZMax;
    }
}

float plane_distance(uint32_t plane, const ClipVertex& v, float x_extent) {
    switch (plane) {
        case PlaneXMin: return v.x + x_extent * v.w;
        case PlaneXMax: return x_extent * v.w - v.x;
        case PlaneYMin: return v.y + v.w;
        case PlaneYMax: return v.w - v.y;
        case PlaneZMin: return v.z + v.w;
        default: return v.w - v.z;
    }
}

void transform_row(const float m[16], const float v[4], float out[4]) {
    for (uint32_t j = 0; j < 4u; ++j) {
        out[j] = m[0 * 4u + j] * v[0] + m[1 * 4u + j] * v[1] + m[2 * 4u + j] * v[2] +
                 m[3 * 4u + j] * v[3];
    }
}

void transform_direction(const float m[16], const float d[3], float out[3]) {
    for (uint32_t j = 0; j < 3u; ++j) {
        out[j] = m[j * 4u + 0] * d[0] + m[j * 4u + 1] * d[1] + m[j * 4u + 2] * d[2];
    }
}

void normalize_safe(float v[3]) {
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length > 0.0f) {
        v[0] /= length;
        v[1] /= length;
        v[2] /= length;
    }
}

ClipVertex lerp_vertex(const ClipVertex& a, const ClipVertex& b, float t) {
    ClipVertex out;
    out.x = a.x + (b.x - a.x) * t;
    out.y = a.y + (b.y - a.y) * t;
    out.z = a.z + (b.z - a.z) * t;
    out.w = a.w + (b.w - a.w) * t;
    out.r = a.r + (b.r - a.r) * t;
    out.g = a.g + (b.g - a.g) * t;
    out.b = a.b + (b.b - a.b) * t;
    out.a = a.a + (b.a - a.a) * t;
    out.s = a.s + (b.s - a.s) * t;
    out.t = a.t + (b.t - a.t) * t;
    return out;
}

}  // namespace

Rsp::Rsp() : geo_log_enabled_(std::getenv("WETTER_GEO_LOG") != nullptr) {
    for (uint32_t i = 0; i < ModelViewStackSize; ++i) {
        for (uint32_t k = 0; k < 16u; ++k) modelview_[i].m[k] = (k % 5u == 0u) ? 1.0f : 0.0f;
    }
    for (uint32_t i = 0; i < ProjectionStackSize; ++i) {
        for (uint32_t k = 0; k < 16u; ++k) projection_[i].m[k] = (k % 5u == 0u) ? 1.0f : 0.0f;
        projection_[i].valid = true;
    }
    for (uint32_t i = 0; i < ModelViewStackSize; ++i) modelview_[i].valid = true;
}

void Rsp::begin_task() {
    static const bool persist = std::getenv("WETTER_PERSIST_MATRICES") != nullptr;
    if (persist) return;

    for (uint32_t i = 0; i < ModelViewStackSize; ++i) {
        for (uint32_t k = 0; k < 16u; ++k) modelview_[i].m[k] = (k % 5u == 0u) ? 1.0f : 0.0f;
        modelview_[i].valid = true;
    }
    for (uint32_t i = 0; i < ProjectionStackSize; ++i) {
        for (uint32_t k = 0; k < 16u; ++k) projection_[i].m[k] = (k % 5u == 0u) ? 1.0f : 0.0f;
        projection_[i].valid = true;
    }
    modelview_depth_ = 0;
    projection_depth_ = 0;
    forced_pending_ = false;
}

void Rsp::set_segment(uint32_t segment, uint32_t address) {
    segments_[segment & 0xFu] = address;
}

uint32_t Rsp::resolve(uint32_t address) const {
    return (segments_[(address >> 24) & 0x0Fu] + (address & 0x00FFFFFFu)) & 0x00FFFFF8u;
}

void Rsp::read_matrix(uint32_t address, Matrix& out) const {
    const uint32_t base = resolve(address);
    if (base + 64u > RdramSize) {
        out.valid = false;
        return;
    }

    for (uint32_t i = 0; i < 4u; ++i) {
        for (uint32_t j = 0; j < 4u; ++j) {
            const uint32_t element = i * 4u + j;
            out.m[element] = read_fixed(rdram_, base + element * 2u, base + 32u + element * 2u);
        }
    }
    out.valid = true;
}

void Rsp::matrix(uint32_t address, uint32_t params) {
    ++stats_.matrices;

    Matrix incoming;
    read_matrix(address, incoming);
    if (geo_log_enabled_) log_matrix(address, params, incoming);
    if (!incoming.valid) return;

    const bool projection = (params & 0x1u) != 0;
    const bool load = (params & 0x2u) != 0;
    const bool push = (params & 0x4u) != 0;

    if (projection) {
        Matrix& top = projection_[projection_depth_];
        top.source = address;
        if (load) {
            top = incoming;
            top.source = address;
        } else {
            Matrix product;
            product.valid = true;
            for (uint32_t i = 0; i < 4u; ++i) {
                for (uint32_t j = 0; j < 4u; ++j) {
                    float sum = 0.0f;
                    for (uint32_t k = 0; k < 4u; ++k) sum += incoming.m[i * 4u + k] * top.m[k * 4u + j];
                    product.m[i * 4u + j] = sum;
                }
            }
            top = product;
            top.source = address;
        }
        return;
    }

    if (push) {
        if (modelview_depth_ + 1u < ModelViewStackSize) {
            modelview_[modelview_depth_ + 1u] = modelview_[modelview_depth_];
            ++modelview_depth_;
        } else {
            ++stats_.stack_overflows;
        }
    }

    Matrix& top = modelview_[modelview_depth_];
    if (load) {
        top = incoming;
    } else {
        Matrix product;
        product.valid = true;
        for (uint32_t i = 0; i < 4u; ++i) {
            for (uint32_t j = 0; j < 4u; ++j) {
                float sum = 0.0f;
                for (uint32_t k = 0; k < 4u; ++k) sum += incoming.m[i * 4u + k] * top.m[k * 4u + j];
                product.m[i * 4u + j] = sum;
            }
        }
        top = product;
    }
    top.source = address;

    if (geo_log_enabled_ && std::getenv("WETTER_MATRIX_LOG") != nullptr) {
        std::fprintf(stderr, "[rsp]   -> modelview depth %u, projection depth %u\n",
                     modelview_depth_, projection_depth_);
        std::fflush(stderr);
    }
}

void Rsp::force_matrix(uint32_t address) {
    ++stats_.force_matrices;
    read_matrix(address, forced_);
    if (geo_log_enabled_) log_matrix(address, 0xFFu, forced_);
    forced_pending_ = forced_.valid;
}

void Rsp::pop_matrix(uint32_t word) {
    if (word != 0u) return;
    if (modelview_depth_ > 0u) {
        --modelview_depth_;
    } else {
        ++stats_.stack_underflows;
    }
}

void Rsp::set_viewport(uint32_t address) {
    ++stats_.viewports;

    const uint32_t base = resolve(address);
    viewport_.scale[0] = static_cast<float>(read_half(rdram_, base)) / 4.0f;
    viewport_.scale[1] = static_cast<float>(read_half(rdram_, base + 2u)) / 4.0f;
    viewport_.scale[2] = static_cast<float>(read_half(rdram_, base + 4u)) / 1024.0f;
    viewport_.translate[0] = static_cast<float>(read_half(rdram_, base + 8u)) / 4.0f;
    viewport_.translate[1] = static_cast<float>(read_half(rdram_, base + 10u)) / 4.0f;
    viewport_.translate[2] = static_cast<float>(read_half(rdram_, base + 12u)) / 1024.0f;

    if (std::getenv("WETTER_MATRIX_LOG") != nullptr) {
        static unsigned logged = 0;
        if (logged < 4u) {
            ++logged;
            std::fprintf(stderr,
                         "[rsp] viewport 0x%06X: scale %.2f %.2f %.4f translate %.2f %.2f %.4f\n",
                         base, static_cast<double>(viewport_.scale[0]),
                         static_cast<double>(viewport_.scale[1]), static_cast<double>(viewport_.scale[2]),
                         static_cast<double>(viewport_.translate[0]),
                         static_cast<double>(viewport_.translate[1]),
                         static_cast<double>(viewport_.translate[2]));
            std::fflush(stderr);
        }
    }
}

void Rsp::set_light(uint32_t index, uint32_t address) {
    if (index >= MaxLights) return;
    ++stats_.lights;

    const uint32_t base = resolve(address);
    Light& light = lights_[index];
    for (uint32_t i = 0; i < 3u; ++i) {
        light.col[i] = static_cast<float>(read_byte(rdram_, base + i)) / 255.0f;
        light.col_copy[i] = static_cast<float>(read_byte(rdram_, base + 4u + i)) / 255.0f;
    }
    light.kc = static_cast<float>(read_byte(rdram_, base + 3u));
    light.kl = static_cast<float>(read_byte(rdram_, base + 7u));
    light.kq = static_cast<float>(read_byte(rdram_, base + 14u));
    for (uint32_t i = 0; i < 3u; ++i) {
        // Signed: a direction component is a signed byte.
        light.dir[i] = static_cast<float>(static_cast<int8_t>(read_byte(rdram_, base + 8u + i)));
    }

    static const bool log = std::getenv("WETTER_LIGHT_LOG") != nullptr;
    if (log && log_window_open_) {
        std::fprintf(stderr,
                     "[rsp] light %u at 0x%06X: colour %.3f %.3f %.3f dir %.1f %.1f %.1f "
                     "(raw kc %u kl %u kq %u)\n",
                     index, base, static_cast<double>(light.col[0]),
                     static_cast<double>(light.col[1]), static_cast<double>(light.col[2]),
                     static_cast<double>(light.dir[0]), static_cast<double>(light.dir[1]),
                     static_cast<double>(light.dir[2]),
                     static_cast<unsigned>(light.kc), static_cast<unsigned>(light.kl),
                     static_cast<unsigned>(light.kq));
        std::fflush(stderr);
    }
}

void Rsp::set_look_at(uint32_t index, uint32_t address) {
    if (index > 1u) return;
    ++stats_.look_ats;

    const uint32_t base = resolve(address);
    float x = static_cast<float>(static_cast<int8_t>(read_byte(rdram_, base + 8u)));
    float y = static_cast<float>(static_cast<int8_t>(read_byte(rdram_, base + 9u)));
    float z = static_cast<float>(static_cast<int8_t>(read_byte(rdram_, base + 10u)));
    const float length = std::sqrt(x * x + y * y + z * z);
    if (length > 0.0f) {
        x /= length;
        y /= length;
        z /= length;
    } else {
        x = y = z = 0.0f;
    }
    look_at_[index][0] = x;
    look_at_[index][1] = y;
    look_at_[index][2] = z;

    static const bool log = std::getenv("WETTER_LIGHT_LOG") != nullptr;
    if (log && log_window_open_) {
        std::fprintf(stderr, "[rsp] lookat %u at 0x%06X: %.4f %.4f %.4f\n", index, base,
                     static_cast<double>(x), static_cast<double>(y), static_cast<double>(z));
        std::fflush(stderr);
    }
}

void Rsp::set_light_count(uint32_t count) {
    light_count_ = (count >> 5u > 0u) ? (count >> 5u) - 1u : 0u;
    if (light_count_ > MaxLights - 1u) light_count_ = MaxLights - 1u;

    static const bool log = std::getenv("WETTER_LIGHT_LOG") != nullptr;
    if (log && log_window_open_) {
        std::fprintf(stderr, "[rsp] numlight: offset 0x%X -> %u light(s) plus an ambient\n",
                     count, light_count_);
        std::fflush(stderr);
    }
}

void Rsp::set_texture(uint32_t tile, uint32_t level, uint32_t on, int32_t sc, int32_t tc) {
    texture_on_ = on != 0u;
    texture_tile_ = tile & 0x7u;
    texture_level_ = level & 0x7u;
    texture_scale_s_ = sc;
    texture_scale_t_ = tc;
}

void Rsp::set_geometry_mode(uint32_t set_bits, uint32_t clear_bits) {
    geometry_mode_ = (geometry_mode_ & ~clear_bits) | set_bits;
}

bool Rsp::texture_enabled() const {
    static const bool force = std::getenv("WETTER_FORCE_TEX") != nullptr;
    if (force) return true;
    return texture_on_;
}

void Rsp::load_vertices(uint32_t address, uint32_t count, uint32_t dst_index) {
    if (count == 0u) return;
    if (dst_index + count > VertexCacheSize) {
        stats_.vertices_rejected += count;
        return;
    }

    const uint32_t base = resolve(address);
    const uint32_t bytes = count * 16u;
    if (base + bytes > RdramSize) {
        stats_.vertices_rejected += count;
        return;
    }

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t at = base + i * 16u;
        RspVertex& v = vertices_[dst_index + i];
        v.x = read_half(rdram_, at);
        v.y = read_half(rdram_, at + 2u);
        v.z = read_half(rdram_, at + 4u);
        v.flag = static_cast<uint16_t>(read_half(rdram_, at + 6u));
        v.s = read_half(rdram_, at + 8u);
        v.t = read_half(rdram_, at + 10u);
        v.r = read_byte(rdram_, at + 12u);
        v.g = read_byte(rdram_, at + 13u);
        v.b = read_byte(rdram_, at + 14u);
        v.a = read_byte(rdram_, at + 15u);
        shade_vertex(v);
        transform_vertex(v);
    }

    stats_.vertices_transformed += count;
}

void Rsp::shade_vertex(RspVertex& v) {
    static const bool log = std::getenv("WETTER_LIGHT_LOG") != nullptr;
    static unsigned logged = 0;

    const bool light_enabled = (geometry_mode_ & 0x20000u) != 0u;      // G_LIGHTING
    const bool texgen_enabled = (geometry_mode_ & 0x40000u) != 0u;     // G_TEXTURE_GEN
    const bool texgen_linear = (geometry_mode_ & 0x80000u) != 0u;      // G_TEXTURE_GEN_LINEAR

    const float normal[3] = { static_cast<float>(static_cast<int8_t>(v.r)) / 127.0f,
                              static_cast<float>(static_cast<int8_t>(v.g)) / 127.0f,
                              static_cast<float>(static_cast<int8_t>(v.b)) / 127.0f };
    const float* world = modelview_[modelview_depth_].m;

    v.colour[0] = static_cast<float>(v.r) / 255.0f;
    v.colour[1] = static_cast<float>(v.g) / 255.0f;
    v.colour[2] = static_cast<float>(v.b) / 255.0f;
    v.alpha = static_cast<float>(v.a) / 255.0f;
    v.lit = false;

    if (light_enabled && light_count_ > 0u) {
        const Light& ambient = lights_[light_count_ < MaxLights ? light_count_ : MaxLights - 1u];
        float result[3] = { ambient.col[0], ambient.col[1], ambient.col[2] };

        for (uint32_t i = 0; i < light_count_; ++i) {
            const Light& light = lights_[i];
            float local_dir[3];
            transform_direction(world, light.dir, local_dir);
            normalize_safe(local_dir);
            float dot = normal[0] * local_dir[0] + normal[1] * local_dir[1] + normal[2] * local_dir[2];
            if (dot < 0.0f) dot = 0.0f;
            for (uint32_t c = 0; c < 3u; ++c) result[c] += dot * light.col[c];
        }

        for (uint32_t c = 0; c < 3u; ++c) {
            v.colour[c] = result[c] > 1.0f ? 1.0f : (result[c] < 0.0f ? 0.0f : result[c]);
        }
        v.lit = true;
        ++stats_.lit_vertices;
    }

    if (log && log_window_open_ && logged < 40u && light_enabled && !texgen_enabled) {
        ++logged;
        std::fprintf(stderr,
                     "[rsp] shade geom 0x%X vertex %d %d %d normal %d %d %d alpha %u -> "
                     "%.3f %.3f %.3f (mv from 0x%06X)\n",
                     geometry_mode_, static_cast<int>(v.x), static_cast<int>(v.y),
                     static_cast<int>(v.z), static_cast<int>(static_cast<int8_t>(v.r)),
                     static_cast<int>(static_cast<int8_t>(v.g)),
                     static_cast<int>(static_cast<int8_t>(v.b)), static_cast<unsigned>(v.a),
                     static_cast<double>(v.colour[0]), static_cast<double>(v.colour[1]),
                     static_cast<double>(v.colour[2]), modelview_[modelview_depth_].source);
        std::fflush(stderr);
    }

    v.generated = false;

    if (light_enabled && texgen_enabled) {
        float axis[2][3];
        for (uint32_t a = 0; a < 2u; ++a) {
            transform_direction(world, look_at_[a], axis[a]);
            normalize_safe(axis[a]);
        }

        float uv[2];
        for (uint32_t a = 0; a < 2u; ++a) {
            float dot = normal[0] * axis[a][0] + normal[1] * axis[a][1] + normal[2] * axis[a][2];
            if (dot < -1.0f) dot = -1.0f;
            if (dot > 1.0f) dot = 1.0f;
            uv[a] = texgen_linear ? static_cast<float>(std::acos(-dot) * 325.94932)
                                  : (dot + 1.0f) * 512.0f;
        }

        const float scale_s = static_cast<float>(static_cast<uint32_t>(texture_scale_s_) & 0xFFFFu);
        const float scale_t = static_cast<float>(static_cast<uint32_t>(texture_scale_t_) & 0xFFFFu);
        v.gen_s = (scale_s / 65536.0f) * uv[0];
        v.gen_t = (scale_t / 65536.0f) * uv[1];
        v.generated = true;
        ++stats_.generated_vertices;

        static const bool texgen_log = std::getenv("WETTER_TEXGEN_LOG") != nullptr;
        static unsigned texgen_logged = 0;
        static const unsigned texgen_limit = [] {
            const char* value = std::getenv("WETTER_TEXGEN_LOG");
            return value != nullptr && *value != '\0' ? static_cast<unsigned>(std::strtoul(value, nullptr, 10))
                                                       : 24u;
        }();
        if (texgen_log && log_window_open_ && texgen_logged < texgen_limit) {
            ++texgen_logged;
            std::fprintf(stderr,
                         "[texgen] %u geom 0x%X normal %.4f %.4f %.4f axis0 %.4f %.4f %.4f "
                         "axis1 %.4f %.4f %.4f dot %.4f %.4f -> uv %.2f %.2f gen %.3f %.3f "
                         "(scale %u %u linear %u tex %u lit %u colour %.3f %.3f %.3f)\n",
                         texgen_logged, geometry_mode_, static_cast<double>(normal[0]),
                         static_cast<double>(normal[1]), static_cast<double>(normal[2]),
                         static_cast<double>(axis[0][0]), static_cast<double>(axis[0][1]),
                         static_cast<double>(axis[0][2]), static_cast<double>(axis[1][0]),
                         static_cast<double>(axis[1][1]), static_cast<double>(axis[1][2]),
                         static_cast<double>((normal[0] * axis[0][0] + normal[1] * axis[0][1] +
                                              normal[2] * axis[0][2])),
                         static_cast<double>((normal[0] * axis[1][0] + normal[1] * axis[1][1] +
                                              normal[2] * axis[1][2])),
                         static_cast<double>(uv[0]), static_cast<double>(uv[1]),
                         static_cast<double>(v.gen_s), static_cast<double>(v.gen_t),
                         static_cast<unsigned>(texture_scale_s_),
                         static_cast<unsigned>(texture_scale_t_), texgen_linear ? 1u : 0u,
                         texture_on_ ? 1u : 0u, v.lit ? 1u : 0u,
                         static_cast<double>(v.colour[0]), static_cast<double>(v.colour[1]),
                         static_cast<double>(v.colour[2]));
            std::fflush(stderr);
        }
    }
}

void Rsp::transform_vertex(RspVertex& v) const {
    const float local[4] = { static_cast<float>(v.x), static_cast<float>(v.y),
                             static_cast<float>(v.z), 1.0f };
    if (forced_pending_) {
        transform_row(forced_.m, local, v.clip);
        return;
    }
    static const bool skip_modelview = std::getenv("WETTER_SKIP_MODELVIEW") != nullptr;
    float eye[4] = { local[0], local[1], local[2], local[3] };
    if (!skip_modelview) transform_row(modelview_[modelview_depth_].m, local, eye);
    transform_row(projection_[projection_depth_].m, eye, v.clip);
}

float Rsp::clip_x_extent() const {
    if (x_extent_ <= 1.0f) return 1.0f;
    const float half = viewport_.scale[0] < 0.0f ? -viewport_.scale[0] : viewport_.scale[0];
    const float left = viewport_.translate[0] - half, right = viewport_.translate[0] + half;
    const float width = static_cast<float>(color_width_);
    return (left <= width * 0.1f && right >= width * 0.9f) ? x_extent_ : 1.0f;
}

int64_t Rsp::screen_z_fixed(uint32_t index) const {
    const RspVertex& v = vertices_[index & (VertexCacheSize - 1u)];
    if (v.clip[3] <= 0.0f) return INT64_MAX;
    const float z = v.clip[2] / v.clip[3];
    // The viewport's z scale and offset are kept divided by 1024.
    const double screen = (static_cast<double>(z) * viewport_.scale[2] + viewport_.translate[2]) * 1024.0;
    return static_cast<int64_t>(screen * 65536.0);
}

void Rsp::compute_clip(uint32_t index, ClipVertex& out) const {
    const RspVertex& v = vertices_[index & (VertexCacheSize - 1u)];

    const float* clip = v.clip;
    out.x = clip[0];
    out.y = clip[1];
    out.z = clip[2];
    out.w = clip[3];

    out.r = v.colour[0];
    out.g = v.colour[1];
    out.b = v.colour[2];
    out.a = v.alpha;

    if (v.generated) {
        out.s = v.gen_s;
        out.t = v.gen_t;
        return;
    }

    const double divisor = 65536.0 * 32.0;
    out.s = static_cast<float>(static_cast<double>(v.s) * static_cast<double>(texture_scale_s_) / divisor);
    out.t = static_cast<float>(static_cast<double>(v.t) * static_cast<double>(texture_scale_t_) / divisor);
}

void Rsp::log_vertex(const ClipVertex& clip, float sx, float sy, float sz) const {
    if (!geo_log_enabled_ || geo_logged_ >= 24u) return;
    ++geo_logged_;
    std::fprintf(stderr,
                 "[rsp] vertex %u: clip %.2f %.2f %.2f %.2f -> screen %.1f %.1f %.4f\n",
                 geo_logged_, static_cast<double>(clip.x), static_cast<double>(clip.y),
                 static_cast<double>(clip.z), static_cast<double>(clip.w), static_cast<double>(sx),
                 static_cast<double>(sy), static_cast<double>(sz));
}

void Rsp::log_matrix(uint32_t address, uint32_t params, const Matrix& m) const {
    if (!geo_log_enabled_ || std::getenv("WETTER_MATRIX_LOG") == nullptr) return;

    static unsigned logged = 0;
    if (logged >= 64u) return;
    ++logged;

    const uint32_t base = resolve(address);

    const bool projection = (params & 0x1u) != 0;
    const bool load = (params & 0x2u) != 0;
    const bool push = (params & 0x4u) != 0;
    std::fprintf(stderr, "[rsp] matrix #%u at 0x%06X (from 0x%08X) params 0x%02X [%s %s %s]\n",
                 logged, base, address, params, projection ? "PROJECTION" : "modelview",
                 load ? "LOAD" : "MUL", push ? "PUSH" : "NOPUSH");
    for (uint32_t i = 0; i < 4u; ++i) {
        std::fprintf(stderr, "[rsp]   %10.4f %10.4f %10.4f %10.4f\n",
                     static_cast<double>(m.m[i * 4u + 0]), static_cast<double>(m.m[i * 4u + 1]),
                     static_cast<double>(m.m[i * 4u + 2]), static_cast<double>(m.m[i * 4u + 3]));
    }

    std::fprintf(stderr, "[rsp]   halfwords");
    for (uint32_t k = 0; k < 32u; ++k) {
        const uint32_t at = base + k * 2u;
        const uint32_t value = (static_cast<uint32_t>(read_byte(rdram_, at)) << 8) |
                               read_byte(rdram_, at + 1u);
        std::fprintf(stderr, " %04X", value);
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

void Rsp::draw_triangle(uint32_t a, uint32_t b, uint32_t c, RdpSink& rdp) {
    ++stats_.triangles_submitted;

    static const bool tex_state = std::getenv("WETTER_TEX_STATE") != nullptr;
    if (tex_state && geo_log_enabled_ && log_window_open_ && tex_state_logged_ < 24u) {
        ++tex_state_logged_;
        std::fprintf(stderr,
                     "[rsp] texstate %u: geom 0x%08X (bit2 %u) gSPTexture on %u tile %u "
                     "level %u scale %d %d -> textured %u\n",
                     tex_state_logged_, geometry_mode_, (geometry_mode_ >> 1) & 1u, texture_on_ ? 1u : 0u,
                     texture_tile_, texture_level_, texture_scale_s_, texture_scale_t_,
                     texture_enabled() ? 1u : 0u);
        std::fflush(stderr);
    }

    // The forced matrix is consumed by the first vertex that uses it, as the
    // microcode does: gSPForceMatrix is a one-shot replacement of the product.
    const bool used_force = forced_pending_;
    ClipVertex poly[3];
    compute_clip(a, poly[0]);
    compute_clip(b, poly[1]);
    compute_clip(c, poly[2]);
    forced_pending_ = false;

    static const bool texgen_probe = std::getenv("WETTER_TEXGEN_LOG") != nullptr;
    static unsigned texgen_draws = 0;
    const bool texgen_draw_wanted =
        texgen_probe && log_window_open_ && texgen_draws < 12u &&
        (geometry_mode_ & 0x40000u) != 0u;
    if (texgen_draw_wanted) {
        ++texgen_draws;
    }

    static const char* tri_log = std::getenv("WETTER_TRI_LOG");
    if (tri_log != nullptr && rdp.frames() == std::strtoull(tri_log, nullptr, 10)) {
        static unsigned tri_logged = 0;
        if (tri_logged < 48u) {
            ++tri_logged;
            const Matrix& mv = modelview_[modelview_depth_];
            const Matrix& pr = projection_[projection_depth_];
            const RspVertex& r0 = vertices_[a & (VertexCacheSize - 1u)];
            const RspVertex& r1 = vertices_[b & (VertexCacheSize - 1u)];
            const RspVertex& r2 = vertices_[c & (VertexCacheSize - 1u)];
            std::fprintf(stderr,
                         "[tri] %u mvd %u prd %u mvsrc 0x%06X prjsrc 0x%06X forced %u "
                         "geom 0x%X idx %u %u %u\n",
                         tri_logged, modelview_depth_, projection_depth_, mv.source, pr.source,
                         used_force ? 1u : 0u, geometry_mode_, a & 0x7Fu, b & 0x7Fu, c & 0x7Fu);
            if (tri_logged <= 8u) {
                for (uint32_t row = 0; row < 4u; ++row) {
                    std::fprintf(stderr, "[tri]   mv  %11.4f %11.4f %11.4f %11.4f\n",
                                 static_cast<double>(mv.m[row * 4u + 0]),
                                 static_cast<double>(mv.m[row * 4u + 1]),
                                 static_cast<double>(mv.m[row * 4u + 2]),
                                 static_cast<double>(mv.m[row * 4u + 3]));
                }
                for (uint32_t row = 0; row < 4u; ++row) {
                    std::fprintf(stderr, "[tri]   prj %11.4f %11.4f %11.4f %11.4f\n",
                                 static_cast<double>(pr.m[row * 4u + 0]),
                                 static_cast<double>(pr.m[row * 4u + 1]),
                                 static_cast<double>(pr.m[row * 4u + 2]),
                                 static_cast<double>(pr.m[row * 4u + 3]));
                }
                if (used_force) {
                    for (uint32_t row = 0; row < 4u; ++row) {
                        std::fprintf(stderr, "[tri]   frc %11.4f %11.4f %11.4f %11.4f\n",
                                     static_cast<double>(forced_.m[row * 4u + 0]),
                                     static_cast<double>(forced_.m[row * 4u + 1]),
                                     static_cast<double>(forced_.m[row * 4u + 2]),
                                     static_cast<double>(forced_.m[row * 4u + 3]));
                    }
                }
            }
            std::fprintf(stderr,
                         "[tri] %u local (%d %d %d) (%d %d %d) (%d %d %d) "
                         "clip (%.1f %.1f %.1f %.1f) (%.1f %.1f %.1f %.1f) (%.1f %.1f %.1f %.1f)\n",
                         tri_logged, r0.x, r0.y, r0.z, r1.x, r1.y, r1.z, r2.x, r2.y, r2.z,
                         static_cast<double>(poly[0].x), static_cast<double>(poly[0].y),
                         static_cast<double>(poly[0].z), static_cast<double>(poly[0].w),
                         static_cast<double>(poly[1].x), static_cast<double>(poly[1].y),
                         static_cast<double>(poly[1].z), static_cast<double>(poly[1].w),
                         static_cast<double>(poly[2].x), static_cast<double>(poly[2].y),
                         static_cast<double>(poly[2].z), static_cast<double>(poly[2].w));
            std::fflush(stderr);
        }
    }

    ClipVertex clipped[3 * (PlaneCount + 1)];
    uint32_t count = 3;
    for (uint32_t i = 0; i < 3u; ++i) clipped[i] = poly[i];

    const ClipMode mode = clip_mode();
    const bool no_clip = mode == ClipMode::None;
    const float x_extent = clip_x_extent();

    for (uint32_t plane = 0; plane < PlaneCount && count > 0u; ++plane) {
        if (!plane_used(mode, plane)) continue;

        // Wholly inside this plane: nothing to cut.
        bool inside = true;
        for (uint32_t i = 0; i < count && inside; ++i) inside = plane_distance(plane, clipped[i], x_extent) >= 0.0f;
        if (inside) continue;

        ClipVertex input[3 * (PlaneCount + 1)];
        const uint32_t input_count = count;
        for (uint32_t i = 0; i < input_count; ++i) input[i] = clipped[i];

        count = 0;
        for (uint32_t i = 0; i < input_count; ++i) {
            const ClipVertex& current = input[i];
            const ClipVertex& next = input[(i + 1u) % input_count];
            const float dc = plane_distance(plane, current, x_extent);
            const float dn = plane_distance(plane, next, x_extent);

            if (dc >= 0.0f) {
                if (count < 3u * (PlaneCount + 1u)) clipped[count++] = current;
            }
            if ((dc >= 0.0f) != (dn >= 0.0f)) {
                const float t = dc / (dc - dn);
                if (count < 3u * (PlaneCount + 1u)) clipped[count++] = lerp_vertex(current, next, t);
            }
        }
        if (count != input_count) ++stats_.triangles_clipped;
    }

    if (count < 3u && !no_clip) {
        ++stats_.triangles_empty;

        static const bool clip_log = std::getenv("WETTER_CLIP_LOG") != nullptr;
        if (clip_log) {
            static unsigned clip_logged = 0;
            if (clip_logged < 6u && poly[0].w > 0.0f) {
                ++clip_logged;

                const RspVertex& r0 = vertices_[a & (VertexCacheSize - 1u)];
                const RspVertex& r1 = vertices_[b & (VertexCacheSize - 1u)];
                const RspVertex& r2 = vertices_[c & (VertexCacheSize - 1u)];
                const Matrix& pr = projection_[projection_depth_];
                std::fprintf(stderr,
                             "[rsp] model %.1f %.1f %.1f | %.1f %.1f %.1f | %.1f %.1f %.1f\n",
                             static_cast<double>(r0.x), static_cast<double>(r0.y),
                             static_cast<double>(r0.z), static_cast<double>(r1.x),
                             static_cast<double>(r1.y), static_cast<double>(r1.z),
                             static_cast<double>(r2.x), static_cast<double>(r2.y),
                             static_cast<double>(r2.z));
                for (uint32_t row = 0; row < 4u; ++row) {
                    const float* m = modelview_[modelview_depth_].m + row * 4u;
                    std::fprintf(stderr, "[rsp] mv  row%u %13.4f %13.4f %13.4f %13.4f\n", row,
                                 static_cast<double>(m[0]), static_cast<double>(m[1]),
                                 static_cast<double>(m[2]), static_cast<double>(m[3]));
                }
                for (uint32_t row = 0; row < 4u; ++row) {
                    const float* m = pr.m + row * 4u;
                    std::fprintf(stderr, "[rsp] prj row%u %13.4f %13.4f %13.4f %13.4f\n", row,
                                 static_cast<double>(m[0]), static_cast<double>(m[1]),
                                 static_cast<double>(m[2]), static_cast<double>(m[3]));
                }
                const Matrix& mv = modelview_[modelview_depth_];
                const float mlocal[4] = { static_cast<float>(r0.x), static_cast<float>(r0.y),
                                          static_cast<float>(r0.z), 1.0f };
                float recheck[4];
                float eye[4];
                transform_row(mv.m, mlocal, eye);
                transform_row(pr.m, eye, recheck);
                std::fprintf(stderr,
                             "[rsp] recomputed (%.3f %.3f %.3f %.3f) from model (%.1f %.1f %.1f) "
                             "and viewproj card %u/%u\n",
                             static_cast<double>(recheck[0]), static_cast<double>(recheck[1]),
                             static_cast<double>(recheck[2]), static_cast<double>(recheck[3]),
                             static_cast<double>(r0.x), static_cast<double>(r0.y),
                             static_cast<double>(r0.z), modelview_depth_, projection_depth_);
                std::fprintf(stderr,
                             "[rsp] view row0 %.3f %.3f %.3f %.3f row1 %.3f %.3f %.3f %.3f\n",
                             static_cast<double>(mv.m[0]), static_cast<double>(mv.m[1]),
                             static_cast<double>(mv.m[2]), static_cast<double>(mv.m[3]),
                             static_cast<double>(mv.m[4]), static_cast<double>(mv.m[5]),
                             static_cast<double>(mv.m[6]), static_cast<double>(mv.m[7]));
                std::fprintf(stderr, "[rsp] mv from 0x%06X, prj from 0x%06X\n",
                             modelview_[modelview_depth_].source, pr.source);
                std::fprintf(stderr,
                             "[rsp] clipped away: (%.3f %.3f %.3f %.3f) (%.3f %.3f %.3f %.3f) "
                             "(%.3f %.3f %.3f %.3f)\n",
                             static_cast<double>(poly[0].x), static_cast<double>(poly[0].y),
                             static_cast<double>(poly[0].z), static_cast<double>(poly[0].w),
                             static_cast<double>(poly[1].x), static_cast<double>(poly[1].y),
                             static_cast<double>(poly[1].z), static_cast<double>(poly[1].w),
                             static_cast<double>(poly[2].x), static_cast<double>(poly[2].y),
                             static_cast<double>(poly[2].z), static_cast<double>(poly[2].w));
                std::fflush(stderr);
            }
        }
        return;
    }

    static const bool culling_disabled = std::getenv("WETTER_NO_CULL") != nullptr;
    const bool cull_front = !culling_disabled && (geometry_mode_ & 0x1000u) != 0u;
    const bool cull_back = !culling_disabled && (geometry_mode_ & 0x2000u) != 0u;

    // Screen vertices for the polygon, with the perspective divide done once and
    // 1/w carried so the rasterizer can interpolate the rest correctly.
    ScreenVertex screen[3 * (PlaneCount + 1)];
    for (uint32_t i = 0; i < count; ++i) {
        const ClipVertex& v = clipped[i];
        const float w = (v.w == 0.0f) ? 1e-6f : v.w;
        const float inv_w = 1.0f / w;
        const float ndc_x = v.x * inv_w;
        const float ndc_y = -v.y * inv_w;
        const float ndc_z = v.z * inv_w;

        screen[i].x = ndc_x * viewport_.scale[0] + viewport_.translate[0];
        screen[i].y = ndc_y * viewport_.scale[1] + viewport_.translate[1];
        screen[i].z = ndc_z * viewport_.scale[2] + viewport_.translate[2];
        screen[i].w = inv_w;
        screen[i].r = v.r;
        screen[i].g = v.g;
        screen[i].b = v.b;
        screen[i].a = v.a;
        screen[i].s = v.s;
        screen[i].t = v.t;

        if (i < 3u) log_vertex(v, screen[i].x, screen[i].y, screen[i].z);
    }

    if (texgen_draw_wanted) {
        rdp.log_texgen_draw(screen, texture_tile_);
    }

    static const bool flat_log = std::getenv("WETTER_FLAT_LOG") != nullptr;
    static unsigned flat_logged = 0;
    static const unsigned flat_limit = [] {
        const char* value = std::getenv("WETTER_FLAT_LOG");
        return value != nullptr && *value != '\0' ? static_cast<unsigned>(std::strtoul(value, nullptr, 10))
                                                   : 40u;
    }();
    if (flat_log && log_window_open_ && flat_logged < flat_limit) {
        const float minx = std::min(std::min(screen[0].x, screen[1].x), screen[2].x);
        const float maxx = std::max(std::max(screen[0].x, screen[1].x), screen[2].x);
        const float miny = std::min(std::min(screen[0].y, screen[1].y), screen[2].y);
        const float maxy = std::max(std::max(screen[0].y, screen[1].y), screen[2].y);
        const uint32_t area = static_cast<uint32_t>((maxx - minx) * (maxy - miny));
        if (area >= 2000u && texture_enabled()) {
            ++flat_logged;
            rdp.log_flat_draw(screen, texture_tile_, geometry_mode_, area);
        }
    }

    for (uint32_t i = 1u; i + 1u < count; ++i) {
        const ScreenVertex& v0 = screen[0];
        const ScreenVertex& v1 = screen[i];
        const ScreenVertex& v2 = screen[i + 1u];

        const float area = (v1.x - v0.x) * (v2.y - v0.y) - (v2.x - v0.x) * (v1.y - v0.y);
        if (area == 0.0f) {
            ++stats_.triangles_degenerate;
            continue;
        }
        if (area > 0.0f && cull_back) {
            ++stats_.triangles_culled;
            continue;
        }
        if (area < 0.0f && cull_front) {
            ++stats_.triangles_culled;
            continue;
        }

        rdp.triangle(v0, v1, v2, texture_enabled(), texture_tile_);
    }
}

}  // namespace wetter::front
