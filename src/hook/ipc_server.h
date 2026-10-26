#pragma once

namespace nj {

// Starts the two pipe server threads (events out, commands in). Each serves one app
// at a time and goes back to listening when it disconnects.
void ipc_start();

} // namespace nj
