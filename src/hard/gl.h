// hard: the OpenGL entry points the GPU backend uses, loaded through the host's
// get_proc_address. One table for OpenGL ES 3.0 and desktop GL 3.3 core: the
// calls below exist in both (glClearDepthf is resolved as glClearDepth on
// desktop GL). Types and constants are the Khronos ones, spelled out here so
// no GL header is needed.
#pragma once

#include <cstddef>
#include <cstdint>

namespace wetter::hard::gl {

using GLenum = unsigned int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLint = int;
using GLuint = unsigned int;
using GLsizei = int;
using GLfloat = float;
using GLchar = char;
using GLubyte = unsigned char;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr = std::ptrdiff_t;

constexpr GLenum NO_ERROR = 0;
constexpr GLenum FALSE = 0;
constexpr GLenum TRUE = 1;
constexpr GLenum TRIANGLES = 0x0004;
constexpr GLenum NEVER = 0x0200, LESS = 0x0201, EQUAL = 0x0202, LEQUAL = 0x0203, GREATER = 0x0204,
                 NOTEQUAL = 0x0205, GEQUAL = 0x0206, ALWAYS = 0x0207;
constexpr GLenum ZERO = 0, ONE = 1, SRC_ALPHA = 0x0302, ONE_MINUS_SRC_ALPHA = 0x0303, DST_ALPHA = 0x0304,
                 ONE_MINUS_DST_ALPHA = 0x0305, CONSTANT_ALPHA = 0x8003, ONE_MINUS_CONSTANT_ALPHA = 0x8004;
constexpr GLenum BLEND = 0x0BE2, DEPTH_TEST = 0x0B71, SCISSOR_TEST = 0x0C11, CULL_FACE = 0x0B44, DITHER = 0x0BD0;
constexpr GLenum UNPACK_ALIGNMENT = 0x0CF5, PACK_ALIGNMENT = 0x0D05;
constexpr GLenum TEXTURE_2D = 0x0DE1, TEXTURE0 = 0x84C0;
constexpr GLenum UNSIGNED_BYTE = 0x1401, FLOAT = 0x1406, UNSIGNED_INT = 0x1405;
constexpr GLenum RGBA = 0x1908, RGBA8 = 0x8058, DEPTH_COMPONENT24 = 0x81A6, DEPTH_COMPONENT16 = 0x81A5;
constexpr GLenum NEAREST = 0x2600, LINEAR = 0x2601;
constexpr GLenum TEXTURE_MAG_FILTER = 0x2800, TEXTURE_MIN_FILTER = 0x2801, TEXTURE_WRAP_S = 0x2802,
                 TEXTURE_WRAP_T = 0x2803;
constexpr GLenum REPEAT = 0x2901, CLAMP_TO_EDGE = 0x812F, MIRRORED_REPEAT = 0x8370;
constexpr GLenum COLOR_BUFFER_BIT = 0x4000, DEPTH_BUFFER_BIT = 0x0100;
constexpr GLenum VENDOR = 0x1F00, RENDERER = 0x1F01, VERSION = 0x1F02, SHADING_LANGUAGE_VERSION = 0x8B8C;
constexpr GLenum ARRAY_BUFFER = 0x8892, STREAM_DRAW = 0x88E0, DYNAMIC_DRAW = 0x88E8;
constexpr GLbitfield MAP_WRITE_BIT = 0x0002, MAP_INVALIDATE_RANGE_BIT = 0x0004, MAP_UNSYNCHRONIZED_BIT = 0x0020;
constexpr GLenum FRAGMENT_SHADER = 0x8B30, VERTEX_SHADER = 0x8B31, COMPILE_STATUS = 0x8B81, LINK_STATUS = 0x8B82,
                 INFO_LOG_LENGTH = 0x8B84;
constexpr GLenum FRAMEBUFFER = 0x8D40, READ_FRAMEBUFFER = 0x8CA8, DRAW_FRAMEBUFFER = 0x8CA9, RENDERBUFFER = 0x8D41,
                 COLOR_ATTACHMENT0 = 0x8CE0, DEPTH_ATTACHMENT = 0x8D00, FRAMEBUFFER_COMPLETE = 0x8CD5;

// X(return type, name, parameters)
#define WETTER_GL_FUNCTIONS(X)                                                                                   \
    X(const GLubyte*, GetString, (GLenum name))                                                                  \
    X(GLenum, GetError, (void))                                                                                  \
    X(void, Enable, (GLenum cap))                                                                                \
    X(void, Disable, (GLenum cap))                                                                               \
    X(void, Viewport, (GLint x, GLint y, GLsizei width, GLsizei height))                                         \
    X(void, Scissor, (GLint x, GLint y, GLsizei width, GLsizei height))                                          \
    X(void, ClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                                            \
    X(void, Clear, (GLbitfield mask))                                                                            \
    X(void, DepthFunc, (GLenum func))                                                                            \
    X(void, DepthMask, (GLboolean flag))                                                                         \
    X(void, ColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a))                                     \
    X(void, BlendFunc, (GLenum sfactor, GLenum dfactor))                                                         \
    X(void, BlendColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                                            \
    X(void, PixelStorei, (GLenum pname, GLint param))                                                            \
    X(void, Finish, (void))                                                                                      \
    X(void, Flush, (void))                                                                                       \
    X(void, GenBuffers, (GLsizei n, GLuint * buffers))                                                           \
    X(void, DeleteBuffers, (GLsizei n, const GLuint* buffers))                                                   \
    X(void, BindBuffer, (GLenum target, GLuint buffer))                                                          \
    X(void, BufferData, (GLenum target, GLsizeiptr size, const void* data, GLenum usage))                        \
    X(void, BufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, const void* data))                  \
    X(void*, MapBufferRange, (GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access))            \
    X(GLboolean, UnmapBuffer, (GLenum target))                                                                   \
    X(void, GenVertexArrays, (GLsizei n, GLuint * arrays))                                                       \
    X(void, BindVertexArray, (GLuint array))                                                                     \
    X(void, VertexAttribPointer,                                                                                 \
      (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer))        \
    X(void, EnableVertexAttribArray, (GLuint index))                                                             \
    X(GLuint, CreateShader, (GLenum type))                                                                       \
    X(void, DeleteShader, (GLuint shader))                                                                       \
    X(void, ShaderSource, (GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length))      \
    X(void, CompileShader, (GLuint shader))                                                                      \
    X(void, GetShaderiv, (GLuint shader, GLenum pname, GLint * params))                                          \
    X(void, GetShaderInfoLog, (GLuint shader, GLsizei max, GLsizei * length, GLchar * log))                      \
    X(GLuint, CreateProgram, (void))                                                                             \
    X(void, AttachShader, (GLuint program, GLuint shader))                                                       \
    X(void, BindAttribLocation, (GLuint program, GLuint index, const GLchar* name))                              \
    X(void, LinkProgram, (GLuint program))                                                                       \
    X(void, GetProgramiv, (GLuint program, GLenum pname, GLint * params))                                        \
    X(void, GetProgramInfoLog, (GLuint program, GLsizei max, GLsizei * length, GLchar * log))                    \
    X(void, UseProgram, (GLuint program))                                                                        \
    X(GLint, GetUniformLocation, (GLuint program, const GLchar* name))                                           \
    X(void, Uniform1i, (GLint location, GLint v0))                                                               \
    X(void, Uniform1f, (GLint location, GLfloat v0))                                                             \
    X(void, Uniform2f, (GLint location, GLfloat v0, GLfloat v1))                                                 \
    X(void, Uniform4f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3))                         \
    X(void, DrawArrays, (GLenum mode, GLint first, GLsizei count))                                               \
    X(void, GenFramebuffers, (GLsizei n, GLuint * framebuffers))                                                 \
    X(void, DeleteFramebuffers, (GLsizei n, const GLuint* framebuffers))                                         \
    X(void, BindFramebuffer, (GLenum target, GLuint framebuffer))                                                \
    X(void, FramebufferTexture2D, (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)) \
    X(void, FramebufferRenderbuffer,                                                                             \
      (GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer))                        \
    X(GLenum, CheckFramebufferStatus, (GLenum target))                                                           \
    X(void, GenRenderbuffers, (GLsizei n, GLuint * renderbuffers))                                               \
    X(void, DeleteRenderbuffers, (GLsizei n, const GLuint* renderbuffers))                                       \
    X(void, BindRenderbuffer, (GLenum target, GLuint renderbuffer))                                              \
    X(void, RenderbufferStorage, (GLenum target, GLenum internalformat, GLsizei width, GLsizei height))          \
    X(void, GenTextures, (GLsizei n, GLuint * textures))                                                         \
    X(void, DeleteTextures, (GLsizei n, const GLuint* textures))                                                 \
    X(void, BindTexture, (GLenum target, GLuint texture))                                                        \
    X(void, ActiveTexture, (GLenum texture))                                                                     \
    X(void, TexImage2D, (GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,       \
                         GLint border, GLenum format, GLenum type, const void* pixels))                          \
    X(void, TexSubImage2D, (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,            \
                            GLsizei height, GLenum format, GLenum type, const void* pixels))                     \
    X(void, TexParameteri, (GLenum target, GLenum pname, GLint param))                                           \
    X(void, BlitFramebuffer, (GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0,     \
                              GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter))                         \
    X(void, ReadPixels,                                                                                          \
      (GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels))

#if defined(_WIN32)
#define WETTER_GL_API __stdcall
#else
#define WETTER_GL_API
#endif

struct Api {
#define WETTER_GL_DECLARE(ret, name, params) ret(WETTER_GL_API* name) params = nullptr;
    WETTER_GL_FUNCTIONS(WETTER_GL_DECLARE)
#undef WETTER_GL_DECLARE
    void(WETTER_GL_API* ClearDepthf)(GLfloat depth) = nullptr;
    void(WETTER_GL_API* ClearDepth)(double depth) = nullptr;   // desktop GL's spelling

    void clear_depth(float depth) const {
        if (ClearDepthf != nullptr) ClearDepthf(depth);
        else if (ClearDepth != nullptr) ClearDepth(depth);
    }
};

// Resolves every entry point; false (and a message naming the first missing
// one) if any is absent.
bool load(Api& api, void* (*get_proc_address)(const char* name));

}  // namespace wetter::hard::gl
