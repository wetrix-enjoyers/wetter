// The host's hooks. See hooks.h.

#include "hooks.h"

namespace wetter::soft {

namespace {

Hooks g_hooks;

}  // namespace

void set_hooks(const Hooks& hooks) { g_hooks = hooks; }
bool host_in_gameplay() { return g_hooks.in_gameplay != nullptr && g_hooks.in_gameplay(); }
uint64_t host_first_gameplay_frame() {
    return g_hooks.first_gameplay_frame != nullptr ? g_hooks.first_gameplay_frame() : 0u;
}
uint64_t host_gameplay_frames() { return g_hooks.gameplay_frames != nullptr ? g_hooks.gameplay_frames() : 0u; }
void host_frame_composed(uint64_t frame) {
    if (g_hooks.frame_composed != nullptr) g_hooks.frame_composed(frame);
}

}  // namespace wetter::soft
