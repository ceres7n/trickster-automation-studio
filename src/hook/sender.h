#pragma once
#include <cstdint>
#include <vector>

namespace nj {

struct SendJob {
    uint32_t reqId = 0;
    uint32_t ctx   = 0;   // connection + 8; 0 = the client's last C2S connection
    uint16_t op    = 0;
    uint16_t param = 0;
    std::vector<uint8_t> data;
};

// Queues a C2S send. It runs on the client's window (game) thread: the game window is
// subclassed and poked with a private message, so the send happens between the client's
// own frames instead of racing them. Without a window it runs on the calling thread.
// The result goes to the app as EV_SEND_RESULT.
void sender_queue(SendJob&& job);

// Runs jobs the game thread has not picked up for a while (called periodically).
void sender_watchdog();

} // namespace nj
