#pragma once
#include <cstdint>

namespace nj {

// True while Trickster Automation Studio itself is sending (CMD_SEND) on this thread, so the C2S hook
// flags the packet as injected.
extern thread_local bool t_injecting;

// Waits (up to timeoutMs) for both hook sites to hold the expected client prologues,
// then installs the C2S + S2C capture hooks. Returns the final HookState.
uint32_t hooks_install(uint32_t timeoutMs);
uint32_t hooks_state();

} // namespace nj
