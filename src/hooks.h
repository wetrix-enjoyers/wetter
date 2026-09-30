// The host's hooks (see wetter::Hooks), with defaults for a host that lends none.
#pragma once

#include <wetter/wetter.h>

namespace wetter::soft {

void set_hooks(const Hooks& hooks);

bool host_in_gameplay();
uint64_t host_first_gameplay_frame();
uint64_t host_gameplay_frames();
void host_frame_composed(uint64_t frame);

}  // namespace wetter::soft
