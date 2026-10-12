// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "j2534.hpp"
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
namespace vci {
using Bytes = std::vector<uint8_t>;
inline uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
struct Writer {
    Bytes b;
    void u8(uint8_t v) {
        b.push_back(v);
    }
    void u16(uint16_t v) {
        u8(v);
        u8(v >> 8);
    }
    void u32(uint32_t v) {
        u16(v);
        u16(v >> 16);
    }
    void u64(uint64_t v) {
        u32(v);
        u32(v >> 32);
    }
    void raw(const void *p, size_t n) {
        if (n) {
            auto a = static_cast<const uint8_t *>(p);
            b.insert(b.end(), a, a + n);
        }
    }
    void msg(const j::Msg &m) {
        if (m.DataSize > 4128)
            throw std::runtime_error("Message too large");
        u32(m.ProtocolID);
        u32(m.RxStatus);
        u32(m.TxFlags);
        u32(m.Timestamp);
        u32(m.DataSize);
        u32(m.ExtraDataIndex);
        raw(m.Data, m.DataSize);
    }
};
struct Reader {
    const Bytes &b;
    size_t at = 0;
    void need(size_t n) const {
        if (n > b.size() - at)
            throw std::runtime_error("Truncated wire body");
    }
    uint8_t u8() {
        need(1);
        return b[at++];
    }
    uint16_t u16() {
        uint16_t v = u8();
        return v | (uint16_t(u8()) << 8);
    }
    uint32_t u32() {
        uint32_t v = u16();
        return v | (uint32_t(u16()) << 16);
    }
    uint64_t u64() {
        uint64_t v = u32();
        return v | (uint64_t(u32()) << 32);
    }
    Bytes raw(size_t n) {
        need(n);
        Bytes v(b.begin() + at, b.begin() + at + n);
        at += n;
        return v;
    }
    j::Msg msg() {
        j::Msg m{};
        m.ProtocolID = u32();
        m.RxStatus = u32();
        m.TxFlags = u32();
        m.Timestamp = u32();
        m.DataSize = u32();
        m.ExtraDataIndex = u32();
        if (m.DataSize > 4128)
            throw std::runtime_error("Oversize message");
        auto v = raw(m.DataSize);
        std::memcpy(m.Data, v.data(), v.size());
        return m;
    }
    void end() const {
        if (at != b.size())
            throw std::runtime_error("Trailing wire bytes");
    }
};
struct Packet {
    uint8_t kind = 0;
    uint16_t op = 0;
    uint32_t session = 0, generation = 0, request = 0;
    uint64_t time = 0;
    Bytes body;
};
inline Bytes encode(const Packet &p) {
    Writer w;
    w.raw("CISO", 4);
    w.u8(1);
    w.u8(p.kind);
    w.u16(p.op);
    w.u32(uint32_t(p.body.size()));
    w.u32(p.session);
    w.u32(p.generation);
    w.u32(p.request);
    w.u64(p.time);
    w.raw(p.body.data(), p.body.size());
    return w.b;
}
inline uint32_t be_id(const j::Msg &m) {
    return uint32_t(m.Data[0]) << 24 | uint32_t(m.Data[1]) << 16 | uint32_t(m.Data[2]) << 8 |
           m.Data[3];
}
inline j::Msg event_message(const Packet &p, uint32_t protocol) {
    if (p.op < 256 || p.op > 258)
        throw std::runtime_error("Event has no J2534 message");
    Reader r{p.body};
    r.u32();
    r.u32();
    r.u32();
    auto status = r.u32(), id = r.u32();
    auto ext = r.u8();
    if (r.u8() != 0)
        throw std::runtime_error("Reserved event byte");
    auto n = r.u16();
    if ((status & ~0x19bu) != 0 || id > 0x1fffffffu || (!(status & 0x100) && id > 0x7ff))
        throw std::runtime_error("Invalid RX metadata");
    j::Msg m{};
    m.ProtocolID = protocol;
    m.RxStatus = status;
    m.Timestamp = uint32_t(p.time);
    m.DataSize = 4;
    for (int i = 0; i < 4; i++)
        m.Data[i] = uint8_t(id >> (24 - 8 * i));
    if (status & 0x80)
        m.Data[m.DataSize++] = ext;
    if (p.op == 258) {
        if (n > 4095)
            throw std::runtime_error("Oversize RX");
        auto data = r.raw(n);
        std::memcpy(m.Data + m.DataSize, data.data(), n);
        m.DataSize += n;
    } else if (p.op == 256 && n != 0)
        throw std::runtime_error("TX indication contains payload");
    r.end();
    m.ExtraDataIndex = m.DataSize;
    return m;
}
struct Error : std::runtime_error {
    uint32_t code;
    Error(uint32_t c, const std::string &s) : std::runtime_error(s), code(c) {}
};
inline uint32_t map_ciso(uint32_t result) {
    static const uint32_t m[] = {0, 1, 10, 8, 2, 1, 15, 17, 12, 8, 7, 7};
    return result < 12 ? m[result] : j::FAILED;
}
} // namespace vci
