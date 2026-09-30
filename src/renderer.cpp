// Renderer::create: picks the backend.

#include <wetter/wetter.h>

namespace wetter {

namespace soft {
void set_hooks(const Hooks& hooks);
std::unique_ptr<Renderer> create_soft(uint8_t* rdram, const Options& options);
}  // namespace soft

namespace hard {
std::unique_ptr<Renderer> create_hard(uint8_t* rdram, const Options& options);
}  // namespace hard

std::unique_ptr<Renderer> Renderer::create(Backend backend, uint8_t* rdram, const Options& options,
                                           const Hooks& hooks) {
    soft::set_hooks(hooks);
    switch (backend) {
        case Backend::Soft:
#if defined(WETTER_HAS_SOFT)
            return soft::create_soft(rdram, options);
#else
            return nullptr;
#endif
        case Backend::Hard:
#if defined(WETTER_HAS_HARD)
            return hard::create_hard(rdram, options);
#else
            return nullptr;
#endif
    }
    return nullptr;
}

}  // namespace wetter
