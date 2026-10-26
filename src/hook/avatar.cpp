#include "hook/avatar.h"
#include "hook/client_addr.h"

namespace nj {

namespace {

// The game thread owns the avatar and frees it on a map change: every read is guarded, a torn read is
// harmless (the next one, 100 ms later, fixes it).
bool read_block(uintptr_t addr, void* out, size_t n)
{
    __try {
        memcpy(out, (const void*)addr, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t image_size(uintptr_t base)
{
    const auto* dos = (const IMAGE_DOS_HEADER*)base;
    const auto* nt  = (const IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
    return nt->OptionalHeader.SizeOfImage;
}

} // namespace

bool read_avatar(EvAvatar& out)
{
    out = EvAvatar{};
    const client::Layout* L = client::active();
    if (!L || !L->rvaWorld) return false;
    const uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    static const uint32_t size = image_size(base);
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    out.fileTime = (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;

    uint32_t world = 0, actor = 0, vtbl = 0;
    if (!read_block(base + L->rvaWorld, &world, 4) || !world) return true;
    if (!read_block(world + L->worldAvatar, &actor, 4) || !actor) return true;
    // an object of the client's own classes: its vtable lies in the image
    if (!read_block(actor, &vtbl, 4) || vtbl < base || vtbl >= base + size) return true;
    uint8_t blk[client::kAvatarDest + 4 - client::kAvatarFlags];
    if (!read_block(actor + client::kAvatarFlags, blk, sizeof(blk))) return true;
    out.actor = actor;
    memcpy(&out.flags, blk, 4);
    memcpy(&out.x, blk + (client::kAvatarPos - client::kAvatarFlags), 2);
    memcpy(&out.y, blk + (client::kAvatarPos - client::kAvatarFlags) + 2, 2);
    memcpy(&out.dx, blk + (client::kAvatarDest - client::kAvatarFlags), 2);
    memcpy(&out.dy, blk + (client::kAvatarDest - client::kAvatarFlags) + 2, 2);
    return true;
}

} // namespace nj
