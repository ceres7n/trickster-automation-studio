#pragma once
#include "shared/ipc.h"

namespace nj {

// Reads the local avatar of the matched client build (client_addr.h rvaWorld / worldAvatar) on the calling
// thread. false = no layout matched yet or the build has no avatar address; out.actor = 0 = not in a map.
bool read_avatar(EvAvatar& out);

} // namespace nj
