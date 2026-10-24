// I/O completion ports and the user I/O APC trampoline. Ports are dispatcher objects (ObjType::Queue) whose
// SignalState mirrors the queued packet count, so NtRemoveIoCompletion blocks through the same Wait machinery
// as every other dispatcher object and wakes through SignalChanged.
#include <chrono>

#include "kernel/exports.h"

namespace kernel {

namespace {

struct Packet {
    uint32_t key;
    uint32_t apc_context;
    uint32_t status;
    uint32_t information;
};

struct IoCompletion : Object {
    std::deque<Packet> packets;  // guarded by DispatchLock, like Object::signal
    uint32_t max_threads = 1;    // NtCreateIoCompletion Count, a scheduling hint with no capacity meaning
    IoCompletion() : Object(ObjType::Queue) {}
};

std::shared_ptr<IoCompletion> PortFromHandle(uint32_t handle, uint32_t& status) {
    ObjectRef o = FromHandle(handle);
    if (!o) {
        status = kInvalidHandle;
        return nullptr;
    }
    auto port = std::dynamic_pointer_cast<IoCompletion>(o);
    if (!port) {
        status = kObjectTypeMismatch;
        return nullptr;
    }
    status = kSuccess;
    return port;
}

// the caller holds no lock; the queue and its SignalState change together under the dispatcher lock. The
// outstanding count lives in SignalState only: KQUEUE.CurrentCount (+0x18) counts the threads servicing the
// queue on NT, not its packets, and is left as the guest built it (review note, KQUEUE.CurrentCount)
void PushPacket(const std::shared_ptr<IoCompletion>& port, Packet packet) {
    std::lock_guard<std::mutex> lock(DispatchLock());
    port->packets.push_back(packet);
    SetSignal(*port, int32_t(port->packets.size()));
    SignalChanged();
}

// NTSTATUS to Win32 error for the completion routine, mirroring RtlNtStatusToDosError
uint32_t DosError(uint32_t status) {
    switch (status) {
        case kSuccess: return 0;
        case kPending: return 997;
        case kTimeout: return 1460;
        case kNoSuchFile:
        case kObjectNameNotFound: return 2;
        case kObjectPathNotFound: return 3;
        case kAccessDenied:
        case kCannotDelete:
        case kFileIsADirectory: return 5;
        case kInvalidHandle: return 6;
        case kNoMemory: return 8;
        case kNoMoreFiles: return 18;
        case kEndOfFile: return 38;
        case kNotSupported: return 50;
        case kInvalidParameter: return 87;
        case kNotImplemented: return 120;
        case kBufferTooSmall: return 122;
        case kObjectNameInvalid: return 123;
        case kObjectNameCollision: return 183;
        case kNotADirectory: return 267;
        case 0xC000007F: return 112;  // disk full
        case kDeviceNotConnected: return 1167;
        default: return (status & 0xC0000000) ? 317 : 0;
    }
}

}  // namespace

ObjectRef NewIoCompletion(uint32_t max_threads) {
    auto port = std::make_shared<IoCompletion>();
    port->max_threads = max_threads ? max_threads : 1;
    return port;
}

void PostIoCompletion(const ObjectRef& port, uint32_t key, uint32_t apc_context, uint32_t status,
                      uint32_t information) {
    auto queue = std::dynamic_pointer_cast<IoCompletion>(port);
    if (!queue) return;
    PushPacket(queue, {key, apc_context, status, information});
}

KERNEL_EXPORT(191, NtCreateIoCompletion, 4) {
    uint32_t handle_out = a[0], attrs = a[2], count = a[3];
    if (!handle_out) return kInvalidParameter;
    std::string name = attrs ? ReadAnsiString(R32(attrs + 4)) : std::string();
    if (!name.empty())
        if (ObjectRef existing = FindNamed(name)) {
            if (existing->type != ObjType::Queue) return kObjectTypeMismatch;
            W32(handle_out, NewHandle(existing));
            return kObjectNameExists;
        }
    auto port = std::static_pointer_cast<IoCompletion>(NewIoCompletion(count));
    uint32_t body = mem::AllocSystem(0x28);
    std::memset(HostPtr(body), 0, 0x28);
    G<uint8_t>(body) = 4;       // QueueObject
    G<uint8_t>(body + 2) = 10;  // header plus entry list and counts, in dwords
    W32(body + 0x10, body + 0x10);  // KQUEUE.EntryListHead
    W32(body + 0x14, body + 0x10);
    W32(body + 0x1C, port->max_threads);  // KQUEUE.MaximumCount
    Bind(port, body);
    if (!name.empty()) AddNamed(name, port);
    W32(handle_out, NewHandle(port));
    return kSuccess;
}

KERNEL_EXPORT(227, NtSetIoCompletion, 5) {
    uint32_t status = kSuccess;
    auto port = PortFromHandle(a[0], status);
    if (!port) return status;
    PushPacket(port, {a[1], a[2], a[3], a[4]});
    return kSuccess;
}

KERNEL_EXPORT(223, NtRemoveIoCompletion, 5) {
    uint32_t status = kSuccess;
    auto port = PortFromHandle(a[0], status);
    if (!port) return status;
    int64_t nt_timeout = a[4] ? int64_t(R64(a[4])) : INT64_MAX;
    bool infinite = nt_timeout == INT64_MAX;
    int64_t deadline = infinite ? 0 : ToDeadline(nt_timeout);
    auto now_ns = [] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(DispatchLock());
            if (!port->packets.empty()) {
                Packet packet = port->packets.front();
                port->packets.pop_front();
                SetSignal(*port, int32_t(port->packets.size()));
                if (a[1]) W32(a[1], packet.key);
                if (a[2]) W32(a[2], packet.apc_context);
                if (a[3]) {
                    W32(a[3], packet.status);
                    W32(a[3] + 4, packet.information);
                }
                return kSuccess;
            }
        }
        int64_t wait_arg = INT64_MAX;
        if (!infinite) {
            int64_t remaining = deadline - now_ns();
            if (remaining <= 0) return kTimeout;
            wait_arg = -((remaining + 99) / 100);  // relative, in 100 ns units
        }
        Wait({port}, false, wait_arg, false);
    }
}

// ApcContext carries the guest completion routine itself, with the Win32 overlapped shape
// (error, transferred, overlapped); the status block doubles as the OVERLAPPED, whose first two fields are the
// NTSTATUS and the byte count. A null routine or block has nothing to run.
KERNEL_EXPORT(232, NtUserIoApcDispatcher, 3) {
    uint32_t routine = a[0], iosb = a[1];
    if (!routine || !iosb) {
        LOG_FIRST(8, "NtUserIoApcDispatcher: routine %08X block %08X", routine, iosb);
        return 0;
    }
    uint32_t status = R32(iosb), information = R32(iosb + 4);
    uint32_t error = 0, transferred = 0;
    if (int32_t(status) >= 0)
        transferred = information;
    else
        error = DosError(status);
    CallGuest(ctx, routine, {error, transferred, iosb});
    return 0;
}

}  // namespace kernel
