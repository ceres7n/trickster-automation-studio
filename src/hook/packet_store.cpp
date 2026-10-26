#include "hook/packet_store.h"
#include <cstdarg>

namespace nj {

namespace {
size_t record_cost(const Record& r) { return r.data.size() + sizeof(Record) + 32; }

struct ExclusiveLock {
    explicit ExclusiveLock(SRWLOCK& l) : l_(l) { AcquireSRWLockExclusive(&l_); }
    ~ExclusiveLock() { ReleaseSRWLockExclusive(&l_); }
    SRWLOCK& l_;
};
} // namespace

PacketStore::PacketStore()
{
    wake_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
}

void PacketStore::push_packet(uint8_t dir, uint8_t flags, uint16_t op, uint16_t param,
                              uint32_t ctx, uint32_t size, std::vector<uint8_t>&& data)
{
    auto rec = std::make_shared<Record>();
    rec->hdr.dir     = dir;
    rec->hdr.flags   = flags;
    rec->hdr.opcode  = op;
    rec->hdr.param   = param;
    rec->hdr.ctx     = ctx;
    rec->hdr.size    = size;
    rec->hdr.dataLen = (uint32_t)data.size();
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    rec->hdr.fileTime = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    rec->data = std::move(data);

    if (dir == DIR_C2S) lastC2SCtx_ = ctx;
    {
        ExclusiveLock g(lock_);
        rec->hdr.seq = nextSeq_++;
        bytes_ += record_cost(*rec);
        recs_.push_back(std::move(rec));
        while (bytes_ > kHistoryBytes && recs_.size() > 1) {
            bytes_ -= record_cost(*recs_.front());
            recs_.pop_front();
        }
    }
    SetEvent(wake_);
}

void PacketStore::post_event(uint16_t type, const void* body, uint32_t len)
{
    Event e{ type, std::vector<uint8_t>(static_cast<const uint8_t*>(body),
                                        static_cast<const uint8_t*>(body) + len) };
    {
        ExclusiveLock g(lock_);
        events_.push_back(std::move(e));
        while (events_.size() > kMaxEvents) events_.pop_front();
    }
    SetEvent(wake_);
}

void PacketStore::collect(uint64_t& from, std::vector<RecordPtr>& out, size_t maxBytes, uint64_t& lost)
{
    lost = 0;
    ExclusiveLock g(lock_);
    const uint64_t front = recs_.empty() ? nextSeq_ : recs_.front()->hdr.seq;
    if (from < front) {
        lost = front - from;
        from = front;
    }
    size_t taken = 0;
    for (size_t i = size_t(from - front); i < recs_.size() && taken < maxBytes; ++i) {
        taken += recs_[i]->data.size() + sizeof(EvPacket);
        out.push_back(recs_[i]);
        ++from;
    }
}

void PacketStore::take_events(std::vector<Event>& out)
{
    ExclusiveLock g(lock_);
    while (!events_.empty()) {
        out.push_back(std::move(events_.front()));
        events_.pop_front();
    }
}

uint64_t PacketStore::next_seq()
{
    ExclusiveLock g(lock_);
    return nextSeq_;
}

PacketStore& store()
{
    static PacketStore s;
    return s;
}

void dll_log(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;

    char dbg[1100];
    snprintf(dbg, sizeof(dbg), "[tas] %s\n", buf);
    OutputDebugStringA(dbg);
    store().post_event(EV_LOG, buf, (uint32_t)n);
}

} // namespace nj
