// hard: the GL loader. See gl.h.

#include "gl.h"

#include <cstdio>

namespace wetter::hard::gl {

bool load(Api& api, void* (*get_proc_address)(const char* name)) {
    if (get_proc_address == nullptr) {
        std::fprintf(stderr, "[hard] no get_proc_address from the host\n");
        return false;
    }
    bool ok = true;
#define WETTER_GL_LOAD(ret, name, params)                                                         \
    api.name = reinterpret_cast<ret(WETTER_GL_API*) params>(get_proc_address("gl" #name));        \
    if (api.name == nullptr && ok) {                                                              \
        std::fprintf(stderr, "[hard] GL entry point gl%s missing\n", #name);                      \
        ok = false;                                                                               \
    }
    WETTER_GL_FUNCTIONS(WETTER_GL_LOAD)
#undef WETTER_GL_LOAD
    api.ClearDepthf = reinterpret_cast<void(WETTER_GL_API*)(GLfloat)>(get_proc_address("glClearDepthf"));
    api.ClearDepth = reinterpret_cast<void(WETTER_GL_API*)(double)>(get_proc_address("glClearDepth"));
    if (api.ClearDepthf == nullptr && api.ClearDepth == nullptr && ok) {
        std::fprintf(stderr, "[hard] GL entry point glClearDepth(f) missing\n");
        ok = false;
    }
    std::fflush(stderr);
    return ok;
}

}  // namespace wetter::hard::gl
