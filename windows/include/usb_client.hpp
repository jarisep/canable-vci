// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "wire.hpp"
#include <atomic>
#include <bcrypt.h>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <memory>
#include <mutex>
#include <setupapi.h>
#include <thread>
#include <windows.h>
#include <winusb.h>
namespace vci {
inline uint64_t random64() {
    uint64_t x = 0;
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&x), sizeof(x),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ||
        !x)
        throw std::runtime_error("Random generator failed");
    return x;
}
struct UsbReply {
    uint32_t request;
    Bytes body;
};
class Client {
  public:
    virtual ~Client() = default;
    virtual void open() = 0;
    virtual void close() = 0;
    virtual bool healthy() const = 0;
    virtual UsbReply command(uint16_t op, const Bytes &body = {}, uint32_t generation = 0) = 0;
    virtual bool pop(Packet &p) = 0;
    virtual void inject(const Packet &) {
        throw Error(j::NOT_SUPPORTED, "Simulation is disabled");
    }
    virtual void mode(uint32_t, uint32_t) {
        throw Error(j::NOT_SUPPORTED, "Simulation is disabled");
    }
};
class UsbClient final : public Client {
    HANDLE file = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE usb = nullptr;
    UCHAR in = 0, out = 0;
    std::wstring serial;
    std::thread reader;
    std::atomic<bool> alive{false}, owned{false};
    std::mutex mutex, commands;
    std::condition_variable cv;
    std::deque<Packet> responses, events;
    std::string failure;
    uint32_t session = 0, request = 0;
    void fail(const std::string &s) {
        std::lock_guard<std::mutex> l(mutex);
        failure = s;
        alive = false;
        cv.notify_all();
    }
    void read_loop() {
        try {
            Bytes bytes;
            while (alive) {
                uint8_t chunk[64];
                ULONG got = 0;
                if (!WinUsb_ReadPipe(usb, in, chunk, sizeof(chunk), &got, nullptr)) {
                    auto e = GetLastError();
                    if (e == ERROR_SEM_TIMEOUT || e == ERROR_TIMEOUT)
                        continue;
                    throw std::runtime_error("WinUSB IN failed: " + std::to_string(e));
                }
                bytes.insert(bytes.end(), chunk, chunk + got);
                while (bytes.size() >= 32) {
                    Bytes header(bytes.begin(), bytes.begin() + 32);
                    Reader r{header};
                    auto magic = r.raw(4);
                    auto version = r.u8(), kind = r.u8();
                    Packet p;
                    p.kind = kind;
                    p.op = r.u16();
                    auto n = r.u32();
                    p.session = r.u32();
                    p.generation = r.u32();
                    p.request = r.u32();
                    p.time = r.u64();
                    if (std::memcmp(magic.data(), "CISO", 4) || version != 1 || kind < 1 ||
                        kind > 3 || n > 4160) {
                        if (owned)
                            throw std::runtime_error("Invalid CISO header; reconnect required");
                        bytes.erase(bytes.begin());
                        continue;
                    }
                    if (bytes.size() < 32 + n)
                        break;
                    p.body.assign(bytes.begin() + 32, bytes.begin() + 32 + n);
                    bytes.erase(bytes.begin(), bytes.begin() + 32 + n);
                    std::lock_guard<std::mutex> l(mutex);
                    if (owned && (kind == 1 || (kind == 3 && p.session != session)))
                        throw std::runtime_error("Invalid owned USB message; reconnect required");
                    auto &q = kind == 3 ? events : responses;
                    if (!owned && kind == 3)
                        continue;
                    if (q.size() >= 64)
                        throw std::runtime_error("Bounded USB queue overflow");
                    q.push_back(std::move(p));
                    cv.notify_all();
                }
            }
        } catch (const std::exception &e) {
            fail(e.what());
        }
    }

  public:
    explicit UsbClient(std::wstring unit) : serial(std::move(unit)) {}
    ~UsbClient() {
        close();
    }
    bool healthy() const override {
        return alive;
    }
    void open() override {
        static const GUID guid = {
            0x5d4e7c68, 0x9bc1, 0x4e2b, {0x9d, 0x68, 0x5c, 0x9a, 0xb8, 0x01, 0xd9, 0xf2}};
        auto set =
            SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == INVALID_HANDLE_VALUE)
            throw Error(j::DEVICE_NOT_CONNECTED, "SetupAPI enumeration failed");
        std::vector<std::wstring> paths;
        for (DWORD index = 0;; index++) {
            SP_DEVICE_INTERFACE_DATA d{};
            d.cbSize = sizeof(d);
            if (!SetupDiEnumDeviceInterfaces(set, nullptr, &guid, index, &d))
                break;
            DWORD size = 0;
            SetupDiGetDeviceInterfaceDetailW(set, &d, nullptr, 0, &size, nullptr);
            std::vector<uint8_t> raw(size);
            auto detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(raw.data());
            detail->cbSize = sizeof(*detail);
            if (SetupDiGetDeviceInterfaceDetailW(set, &d, detail, size, nullptr, nullptr)) {
                std::wstring path = detail->DevicePath, lower = path, unit = serial;
                for (auto &c : lower)
                    c = towlower(c);
                for (auto &c : unit)
                    c = towlower(c);
                if (lower.find(L"vid_1209&pid_0001") != std::wstring::npos &&
                    lower.find(unit) != std::wstring::npos)
                    paths.push_back(path);
            }
        }
        SetupDiDestroyDeviceInfoList(set);
        if (paths.size() != 1)
            throw Error(j::DEVICE_NOT_CONNECTED, "Expected one CANable with the configured serial");
        file = CreateFileW(paths[0].c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
        if (file == INVALID_HANDLE_VALUE || !WinUsb_Initialize(file, &usb)) {
            close();
            throw Error(j::DEVICE_NOT_CONNECTED, "WinUSB interface open failed");
        }
        USB_INTERFACE_DESCRIPTOR desc{};
        if (!WinUsb_QueryInterfaceSettings(usb, 0, &desc) || desc.bInterfaceClass != 255 ||
            desc.bInterfaceSubClass != 73 || desc.bInterfaceProtocol != 1) {
            close();
            throw Error(j::DEVICE_NOT_CONNECTED, "Wrong USB interface identity");
        }
        for (UCHAR i = 0; i < desc.bNumEndpoints; i++) {
            WINUSB_PIPE_INFORMATION p{};
            if (WinUsb_QueryPipe(usb, 0, i, &p) && p.PipeType == UsbdPipeTypeBulk) {
                if (p.PipeId & 0x80)
                    in = p.PipeId;
                else
                    out = p.PipeId;
            }
        }
        if (!in || !out) {
            close();
            throw Error(j::DEVICE_NOT_CONNECTED, "Missing bulk endpoints");
        }
        ULONG timeout = 100;
        WinUsb_SetPipePolicy(usb, in, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);
        timeout = 2000;
        WinUsb_SetPipePolicy(usb, out, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);
        {
            std::lock_guard<std::mutex> l(mutex);
            failure.clear();
            events.clear();
            responses.clear();
        }
        request = uint32_t(random64() & 0x7fffffffu);
        owned = false;
        session = 0;
        alive = true;
        reader = std::thread([this] { read_loop(); });
        try {
            Writer w;
            auto nonce = random64();
            w.u64(nonce);
            w.u32(5000);
            w.u32(0);
            auto reply = command(1, w.b);
            Reader r{reply.body};
            r.u32();
            if (r.u64() != nonce)
                throw std::runtime_error("HELLO nonce differs");
            r.u64();
            session = r.u32();
            if (r.u32() != 5000 || r.u32() != 1 || !session)
                throw std::runtime_error("HELLO capability/lease mismatch");
            r.end();
            owned = true;
            auto caps = command(2);
            Reader c{caps.body};
            c.u32();
            if ((c.u64() & 0x7f) != 0x7f || c.u32() != 4095)
                throw std::runtime_error("Firmware capabilities differ from stage 2 contract");
        } catch (...) {
            close();
            throw;
        }
    }
    UsbReply command(uint16_t op, const Bytes &body = {}, uint32_t gen = 0) override {
        std::unique_lock<std::mutex> serial_lock(commands);
        if (!alive)
            throw Error(j::DEVICE_NOT_CONNECTED, "USB session is lost");
        if (request == 0xffffffffu)
            throw Error(j::DEVICE_NOT_CONNECTED, "Request ID exhausted; reopen");
        Packet p{1, op, op == 1 ? 0 : session, gen, ++request, 0, body};
        auto data = encode(p);
        ULONG wrote = 0;
        if (!WinUsb_WritePipe(usb, out, data.data(), ULONG(data.size()), &wrote, nullptr) ||
            wrote != data.size()) {
            fail("WinUSB OUT failed; TX acceptance is uncertain");
            throw Error(j::DEVICE_NOT_CONNECTED, "USB write failed; no automatic replay");
        }
        std::unique_lock<std::mutex> l(mutex);
        auto end = now_ms() + 2500;
        for (;;) {
            while (!responses.empty()) {
                auto reply = std::move(responses.front());
                responses.pop_front();
                if (reply.request != p.request || reply.op != op) {
                    if (!owned)
                        continue;
                    failure = "Unexpected USB response; reconnect required";
                    alive = false;
                    cv.notify_all();
                    throw Error(j::DEVICE_NOT_CONNECTED, failure);
                }
                if (reply.generation != gen || reply.session != p.session ||
                    reply.body.size() < 4) {
                    failure = "USB response ownership/body differs; reconnect required";
                    alive = false;
                    cv.notify_all();
                    throw Error(j::DEVICE_NOT_CONNECTED, failure);
                }
                Reader r{reply.body};
                auto result = r.u32();
                if (result)
                    throw Error(map_ciso(result), "CISO command " + std::to_string(op) +
                                                      " returned " + std::to_string(result));
                return {p.request, std::move(reply.body)};
            }
            if (!alive)
                throw Error(j::DEVICE_NOT_CONNECTED, failure);
            if (now_ms() >= end) {
                alive = false;
                cv.notify_all();
                throw Error(j::DEVICE_NOT_CONNECTED, "USB command timed out; no replay");
            }
            cv.wait_for(l, std::chrono::milliseconds(20));
        }
    }
    bool pop(Packet &p) override {
        std::lock_guard<std::mutex> l(mutex);
        if (events.empty())
            return false;
        p = std::move(events.front());
        events.pop_front();
        return true;
    }
    void close() override {
        alive = false;
        if (reader.joinable())
            reader.join();
        if (usb) {
            WinUsb_Free(usb);
            usb = nullptr;
        }
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            file = INVALID_HANDLE_VALUE;
        }
        session = 0;
        owned = false;
        in = out = 0;
    }
};
class FakeClient final : public Client {
    std::mutex m;
    std::deque<Packet> events;
    std::atomic<bool> alive{false};
    uint32_t request = 0, seq = 0, gen = 0, route = 0, flags = 0, id = 0;
    uint8_t ext = 0;
    uint32_t error = 0, delay = 0;

  public:
    void open() override {
        alive = true;
        request = seq = gen = route = error = delay = 0;
    }
    void close() override {
        alive = false;
        std::lock_guard<std::mutex> l(m);
        events.clear();
    }
    bool healthy() const override {
        return alive;
    }
    UsbReply command(uint16_t op, const Bytes &b = {}, uint32_t g = 0) override {
        if (!alive)
            throw Error(j::DEVICE_NOT_CONNECTED, "Simulated USB loss");
        Reader r{b};
        Writer w;
        w.u32(0);
        auto req = ++request;
        if (op == 5) {
            uint8_t uid[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
            w.raw(uid, 12);
            char version[20] = "sim-0.3.0", revision[20] = "offline-only", board[16] = "simulated";
            w.raw(version, 20);
            w.raw(revision, 20);
            w.raw(board, 16);
        }
        if (op == 16)
            gen = g;
        if (op == 32) {
            route = r.u32();
            r.u32();
            id = r.u32();
            auto addressing = r.u32();
            r.u32();
            r.u8();
            ext = r.u8();
            flags = (addressing & 1 ? 0x80 : 0) | (addressing & 2 ? 0x100 : 0);
        }
        if (op == 48) {
            r.u32();
            r.u32();
            r.u16();
            r.u16();
            r.u32();
            if (delay)
                Sleep(delay);
            Writer e;
            e.u32(++seq);
            e.u32(route);
            e.u32(req);
            e.u32(error ? 0 : 9 | flags);
            e.u32(id);
            e.u8(ext);
            e.u8(0);
            e.u16(0);
            if (error) {
                e.u32(error);
                e.u32(uint32_t(-1));
            }
            Packet p{3, uint16_t(error ? 259 : 256), 1, g, req, now_ms() * 1000, e.b};
            inject(p);
        }
        if (op == 17 || op == 33 || op == 49 || op == 50)
            w.u32(seq);
        return {req, w.b};
    }
    bool pop(Packet &p) override {
        std::lock_guard<std::mutex> l(m);
        if (events.empty())
            return false;
        p = std::move(events.front());
        events.pop_front();
        return true;
    }
    void inject(const Packet &input) override {
        std::lock_guard<std::mutex> l(m);
        if (events.size() >= 64)
            throw Error(j::BUFFER_OVERFLOW, "Fake event queue full");
        auto p = input;
        Reader r{p.body};
        auto supplied = r.u32();
        if (!supplied) {
            auto s = ++seq;
            for (int i = 0; i < 4; i++)
                p.body[i] = uint8_t(s >> (i * 8));
        } else
            seq = std::max(seq, supplied);
        events.push_back(std::move(p));
    }
    void mode(uint32_t what, uint32_t value) override {
        if (what == 0)
            alive = value != 0;
        else if (what == 1)
            error = value;
        else if (what == 2)
            delay = std::min(value, 1000u);
        else
            throw Error(j::INVALID_IOCTL_VALUE, "Unknown simulation mode");
    }
};
} // namespace vci
