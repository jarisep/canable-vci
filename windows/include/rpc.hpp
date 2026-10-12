// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "wire.hpp"
#include <algorithm>
#include <atomic>
namespace vci {
enum RpcOp : uint16_t {
    OPEN = 1,
    CLOSE = 2,
    CONNECT = 3,
    DISCONNECT = 4,
    READ = 5,
    WRITE = 6,
    PERIODIC_ADD = 7,
    PERIODIC_STOP = 8,
    FILTER_ADD = 9,
    FILTER_STOP = 10,
    VOLTAGE = 11,
    VERSION = 12,
    CONFIG = 13,
    CLEAR = 14,
    INFO = 15,
    PROGRAM = 16,
    CONTROL = 17,
    TRACE = 18,
    TEST_INJECT = 0x7000,
    TEST_MODE = 0x7001
};
constexpr uint32_t IPC_MAX = 270000;
struct Rpc {
    uint16_t op = 0;
    uint32_t request = 0, result = 0;
    uint64_t owner = 0;
    Bytes body;
};
inline Bytes rpc_header(const Rpc &p) {
    Writer w;
    w.raw("CVP1", 4);
    w.u16(1);
    w.u16(p.op);
    w.u32(uint32_t(p.body.size()));
    w.u32(p.request);
    w.u32(p.result);
    w.u32(0);
    w.u64(p.owner);
    return w.b;
}
inline bool pipe_io(HANDLE h, void *data, size_t n, bool write, uint32_t timeout = 5000) {
    auto p = static_cast<uint8_t *>(data);
    uint64_t end = now_ms() + timeout;
    while (n) {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent)
            return false;
        DWORD got = 0;
        BOOL ok =
            write ? WriteFile(h, p, DWORD(n), &got, &ov) : ReadFile(h, p, DWORD(n), &got, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            DWORD wait =
                timeout == INFINITE
                    ? INFINITE
                    : DWORD(std::min<uint64_t>(MAXDWORD, end > now_ms() ? end - now_ms() : 0));
            if (WaitForSingleObject(ov.hEvent, wait) != WAIT_OBJECT_0) {
                CancelIoEx(h, &ov);
                GetOverlappedResult(h, &ov, &got, TRUE);
                CloseHandle(ov.hEvent);
                return false;
            }
            ok = GetOverlappedResult(h, &ov, &got, FALSE);
        }
        CloseHandle(ov.hEvent);
        if (!ok || !got)
            return false;
        p += got;
        n -= got;
    }
    return true;
}
inline bool rpc_send(HANDLE h, const Rpc &r) {
    auto head = rpc_header(r);
    return pipe_io(h, head.data(), head.size(), true) &&
           pipe_io(h, const_cast<uint8_t *>(r.body.data()), r.body.size(), true);
}
inline bool rpc_receive(HANDLE h, Rpc &p, uint32_t timeout = 5000) {
    Bytes b(32);
    if (!pipe_io(h, b.data(), b.size(), false, timeout))
        return false;
    Reader r{b};
    auto magic = r.raw(4);
    if (std::memcmp(magic.data(), "CVP1", 4) || r.u16() != 1)
        return false;
    p.op = r.u16();
    auto n = r.u32();
    p.request = r.u32();
    p.result = r.u32();
    if (r.u32() != 0 || n > IPC_MAX)
        return false;
    p.owner = r.u64();
    p.body.resize(n);
    return pipe_io(h, p.body.data(), n, false);
}
inline bool rpc_reply(HANDLE h, Rpc &p, uint32_t timeout = 5000) {
    if (!rpc_receive(h, p, timeout))
        return false;
    // Keep a one-shot server pipe connected until its entire reply was consumed.
    // DisconnectNamedPipe would otherwise discard unread buffered reply bytes.
    if (p.op != OPEN) {
        uint8_t ack = 0xa5;
        if (!pipe_io(h, &ack, 1, true))
            return false;
    }
    return true;
}
inline HANDLE pipe_open(const std::wstring &name) {
    for (int i = 0; i < 3; i++) {
        HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h != INVALID_HANDLE_VALUE)
            return h;
        if (GetLastError() != ERROR_PIPE_BUSY)
            break;
        WaitNamedPipeW(name.c_str(), 1000);
    }
    throw Error(j::DEVICE_NOT_CONNECTED, "CANable service pipe is unavailable");
}
} // namespace vci
