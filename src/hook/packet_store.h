#pragma once
#include "shared/ipc.h"
#include <deque>
#include <memory>
#include <vector>

namespace nj {

struct Record {
    EvPacket hdr{};
    std::vector<uint8_t> data;
};
using RecordPtr = std::shared_ptr<const Record>;

struct Event {
    uint16_t type;
    std::vector<uint8_t> body;
};

// Everything the DLL has to tell the app. Captured packets stay as a bounded history,
// so an app that connects late (or reconnects) gets them replayed and can rebuild its
// state from the login burst; other events (logs, send results) queue until read.
class PacketStore {
public:
    static constexpr size_t   kHistoryBytes = 32u << 20;
    static constexpr uint32_t kMaxKeep      = 0x10000;   // payload bytes kept per packet
    static constexpr size_t   kMaxEvents    = 1000;

    PacketStore();

    // Called from the hooks (any client thread).
    void push_packet(uint8_t dir, uint8_t flags, uint16_t op, uint16_t param, uint32_t ctx,
                     uint32_t size, std::vector<uint8_t>&& data);
    void post_event(uint16_t type, const void* body, uint32_t len);

    // Records with seq >= from, up to ~maxBytes of payload; from advances past what was
    // returned. `lost` = records evicted before they were read.
    void collect(uint64_t& from, std::vector<RecordPtr>& out, size_t maxBytes, uint64_t& lost);
    void take_events(std::vector<Event>& out);
    uint64_t next_seq();

    uint32_t last_c2s_ctx() const { return lastC2SCtx_; }
    HANDLE wake_event() const { return wake_; }

private:
    SRWLOCK lock_ = SRWLOCK_INIT;
    std::deque<RecordPtr> recs_;
    size_t bytes_ = 0;
    uint64_t nextSeq_ = 1;
    std::deque<Event> events_;
    volatile uint32_t lastC2SCtx_ = 0;
    HANDLE wake_ = nullptr;
};

PacketStore& store();

// printf-style line to the app's log (and OutputDebugString).
void dll_log(const char* fmt, ...);

} // namespace nj
