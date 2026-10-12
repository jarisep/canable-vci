// SPDX-License-Identifier: Apache-2.0
#include "captured_cases.hpp"
#include "rpc.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace vci;
static unsigned checks = 0;
static void expect(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
    checks++;
}
struct Api {
    HMODULE dll;
    uint32_t(WINAPI *Open)(void *, uint32_t *);
    uint32_t(WINAPI *Close)(uint32_t);
    uint32_t(WINAPI *Connect)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *);
    uint32_t(WINAPI *Disconnect)(uint32_t);
    uint32_t(WINAPI *Read)(uint32_t, j::Msg *, uint32_t *, uint32_t);
    uint32_t(WINAPI *Write)(uint32_t, j::Msg *, uint32_t *, uint32_t);
    uint32_t(WINAPI *PeriodicAdd)(uint32_t, j::Msg *, uint32_t *, uint32_t);
    uint32_t(WINAPI *PeriodicStop)(uint32_t, uint32_t);
    uint32_t(WINAPI *FilterAdd)(uint32_t, uint32_t, j::Msg *, j::Msg *, j::Msg *, uint32_t *);
    uint32_t(WINAPI *FilterStop)(uint32_t, uint32_t);
    uint32_t(WINAPI *Program)(uint32_t, uint32_t, uint32_t);
    uint32_t(WINAPI *Version)(uint32_t, char *, char *, char *);
    uint32_t(WINAPI *Error)(char *);
    uint32_t(WINAPI *Ioctl)(uint32_t, uint32_t, void *, void *);
    explicit Api(const char *path) {
        dll = LoadLibraryA(path);
        if (!dll)
            throw std::runtime_error("DLL load failed: " + std::to_string(GetLastError()));
#define LOAD(field, name)                                                                          \
    field = reinterpret_cast<decltype(field)>(GetProcAddress(dll, name));                          \
    expect(field != nullptr, name)
        LOAD(Open, "PassThruOpen");
        LOAD(Close, "PassThruClose");
        LOAD(Connect, "PassThruConnect");
        LOAD(Disconnect, "PassThruDisconnect");
        LOAD(Read, "PassThruReadMsgs");
        LOAD(Write, "PassThruWriteMsgs");
        LOAD(PeriodicAdd, "PassThruStartPeriodicMsg");
        LOAD(PeriodicStop, "PassThruStopPeriodicMsg");
        LOAD(FilterAdd, "PassThruStartMsgFilter");
        LOAD(FilterStop, "PassThruStopMsgFilter");
        LOAD(Program, "PassThruSetProgrammingVoltage");
        LOAD(Version, "PassThruReadVersion");
        LOAD(Error, "PassThruGetLastError");
        LOAD(Ioctl, "PassThruIoctl");
#undef LOAD
    }
    ~Api() {
        FreeLibrary(dll);
    }
};
static uint64_t owner = 0;
static uint32_t request = 10;
static Rpc fake_call(uint16_t op, const Bytes &body, uint64_t token = owner) {
    const auto deadline = now_ms() + 2000;
    for (;;) {
        auto h = pipe_open(L"\\\\.\\pipe\\CodexCanableVciTest");
        Rpc reply;
        if (!rpc_send(h, {op, ++request, 0, token, body}) || !rpc_reply(h, reply)) {
            CloseHandle(h);
            throw std::runtime_error("Test RPC failed");
        }
        CloseHandle(h);
        // Feed all events to the production RX queue before asserting its bound.
        // A fast producer can temporarily fill the separate fake transport queue.
        // Rejected injection has not enqueued an event, so this retry cannot duplicate it.
        if (op == TEST_INJECT && reply.result == j::BUFFER_OVERFLOW && now_ms() < deadline) {
            Sleep(10);
            continue;
        }
        if (reply.result)
            throw std::runtime_error("Test RPC returned " + std::to_string(reply.result));
        return reply;
    }
}
static void mode(uint32_t key, uint32_t value) {
    Writer w;
    w.u32(key);
    w.u32(value);
    fake_call(TEST_MODE, w.b);
}
static void operator_state(uint32_t key, uint32_t value) {
    Writer w;
    w.u32(key);
    w.u32(value);
    fake_call(CONTROL, w.b, 0);
}
static void inject(const j::Msg &m) {
    Writer e;
    auto offset = m.RxStatus & 0x80 ? 5u : 4u;
    uint16_t op = m.RxStatus & 8 ? 256 : m.RxStatus & 2 ? 257 : 258;
    e.u32(0);
    e.u32(1);
    e.u32(0);
    e.u32(m.RxStatus);
    e.u32(be_id(m));
    e.u8(offset == 5 ? m.Data[4] : 0);
    e.u8(0);
    e.u16(op == 258 ? uint16_t(m.DataSize - offset) : 0);
    if (op == 258)
        e.raw(m.Data + offset, m.DataSize - offset);
    Writer w;
    w.u16(op);
    w.u16(0);
    w.u32(0);
    w.u64(m.Timestamp);
    w.u32(uint32_t(e.b.size()));
    w.raw(e.b.data(), e.b.size());
    fake_call(TEST_INJECT, w.b);
}
static j::Msg msg(uint32_t protocol, uint32_t id, std::initializer_list<uint8_t> data = {}) {
    j::Msg m{};
    m.ProtocolID = protocol;
    m.DataSize = 4;
    for (int i = 0; i < 4; i++)
        m.Data[i] = uint8_t(id >> (24 - 8 * i));
    for (auto b : data)
        m.Data[m.DataSize++] = b;
    return m;
}
int main(int argc, char **argv) {
    try {
        if (argc < 3)
            throw std::runtime_error("Usage: api_test.exe DLL --fake|--real|--owner-crash");
        Api api(argv[1]);
        bool fake = std::string(argv[2]) == "--fake";
        uint32_t device = 0, ch = 0, n = 0, id = 0;
        expect(api.Open(nullptr, nullptr) == j::NULL_PARAMETER, "Open NULL output");
        expect(api.Open(nullptr, &device) == 0, "Open real DLL");
        if (std::string(argv[2]) == "--owner-crash") {
            expect(api.Connect(device, 7, 0, 500000, &ch) == 0, "Crash owner channel");
            std::cout << "Owner opened; process exits without Close\n";
            ExitProcess(0);
        }
        uint32_t second = 0;
        expect(api.Open(nullptr, &second) == j::DEVICE_IN_USE, "Second Open rejected");
        if (std::string(argv[2]) == "--service-loss") {
            expect(api.Connect(device, 7, 0, 500000, &ch) == 0, "Service-loss channel");
            std::ofstream("C:\\codex-canable-vci\\logs\\service-loss-ready.txt")
                << GetCurrentProcessId() << "\n";
            j::Msg m{};
            n = 1;
            auto start = now_ms();
            auto code = api.Read(ch, &m, &n, 30000);
            expect(code == j::DEVICE_NOT_CONNECTED && n == 0 && now_ms() - start < 15000,
                   "Service stop releases blocked reader");
            auto oldChannel = ch;
            auto oldClose = api.Close(device);
            expect(oldClose == j::DEVICE_NOT_CONNECTED, "Broken owner connection invalidated");
            bool reopened = false;
            for (int i = 0; i < 100; i++) {
                if (api.Open(nullptr, &device) == 0) {
                    reopened = true;
                    break;
                }
                Sleep(200);
            }
            expect(reopened, "Fresh Open after service restart");
            j::Config c{0x8001, 0};
            j::ConfigList list{1, &c};
            expect(api.Ioctl(oldChannel, 1, &list, nullptr) == j::INVALID_CHANNEL,
                   "Old channel rejected after restart");
            expect(api.Close(device) == 0, "Fresh owner cleanup");
            std::cout << "SERVICE RESTART PASS " << checks
                      << " checks; no CAN messages transmitted\n";
            return 0;
        }
        if (std::string(argv[2]) == "--usb-loss") {
            expect(api.Connect(device, 7, 0, 500000, &ch) == 0, "USB-loss channel");
            std::ofstream("C:\\codex-canable-vci\\logs\\usb-loss-ready.txt")
                << GetCurrentProcessId() << "\n";
            std::cout << "USB LOSS TEST READY; waiting for physical unplug\n" << std::flush;
            bool lost = false;
            for (int i = 0; i < 60; i++) {
                j::Msg m{};
                n = 1;
                auto code = api.Read(ch, &m, &n, 5000);
                if (code == j::DEVICE_NOT_CONNECTED) {
                    expect(n == 0, "Unplugged read has no invented messages");
                    lost = true;
                    break;
                }
                expect(code == j::TIMEOUT && n == 0, "USB-loss wait remains empty");
            }
            expect(lost, "Physical USB loss reaches blocked reader");
            uint32_t mv = 0;
            expect(api.Ioctl(device, 3, nullptr, &mv) == j::DEVICE_NOT_CONNECTED,
                   "USB loss invalidates battery emulation");
            expect(api.Ioctl(device, 0x800e, nullptr, &mv) == j::DEVICE_NOT_CONNECTED,
                   "USB loss invalidates ignition emulation");
            auto oldChannel = ch;
            expect(api.Close(device) == j::DEVICE_NOT_CONNECTED,
                   "Lost USB owner is released with explicit error");
            std::cout << "USB LOSS OBSERVED; waiting for physical replug\n" << std::flush;
            bool reopened = false;
            for (int i = 0; i < 1500; i++) {
                if (api.Open(nullptr, &device) == 0) {
                    reopened = true;
                    break;
                }
                Sleep(200);
            }
            expect(reopened, "Fresh Open after USB replug");
            j::Config c{0x8001, 0};
            j::ConfigList list{1, &c};
            expect(api.Ioctl(oldChannel, 1, &list, nullptr) == j::INVALID_CHANNEL,
                   "Old channel rejected after USB recovery");
            expect(api.Connect(device, 7, 0, 500000, &ch) == 0, "Fresh channel after USB recovery");
            expect(api.Disconnect(ch) == 0 && api.Close(device) == 0, "USB recovery cleanup");
            std::cout << "PHYSICAL USB LOSS AND RECOVERY PASS " << checks
                      << " checks; no CAN messages transmitted\n";
            return 0;
        }
        char fw[80]{}, dll[80]{}, version[80]{};
        auto versionResult = api.Version(device, fw, dll, version);
        if (versionResult) {
            char error[80]{};
            api.Error(error);
            std::cerr << "ReadVersion result=" << versionResult << " error=" << error << "\n";
        }
        expect(versionResult == 0 && std::string(version) == "04.04", "Version ABI");
        std::cout << "IDENTITY firmware=" << fw << " dll=" << dll << " api=" << version << "\n";
        j::Param serial{1, 0, 0};
        j::ParamList info{1, &serial};
        expect(api.Ioctl(device, 0x800c, nullptr, &info) == 0 && serial.Value &&
                   serial.Supported == 1,
               "Stable own serial");
        std::cout << "SERIAL " << serial.Value << "\n";
        uint32_t voltage = 0;
        expect(api.Ioctl(device, 3, nullptr, &voltage) == 0 && voltage == 13500,
               "Emulated battery");
        expect(api.Ioctl(device, 0x800e, nullptr, &voltage) == 0 && voltage == 13500,
               "Forced ignition ON");
        expect(api.Ioctl(device, 0x0e, nullptr, &voltage) == j::NOT_SUPPORTED,
               "No programming voltage sensor");
        expect(api.Program(device, 0, 0xffffffff) == 0, "PIN_OFF cleanup");
        expect(api.Program(device, 1, 12000) == j::NOT_SUPPORTED,
               "Programming activation rejected");
        if (std::string(argv[2]) == "--identify") {
            expect(api.Close(device) == 0, "Identification cleanup");
            std::cout << "IDENTIFICATION PASS " << checks << " checks\n";
            return 0;
        }
        if (fake) {
            Writer w;
            w.u32(0);
            w.u32(0);
            auto reply = fake_call(CONTROL, w.b, 0);
            Reader r{reply.body};
            for (int i = 0; i < 5; i++)
                r.u32();
            owner = r.u64();
            expect(owner != 0, "Test owner token is bound to current PID");
            operator_state(1, 0);
            expect(api.Ioctl(device, 0x800e, nullptr, &voltage) == 0 && voltage == 0,
                   "Manual ignition OFF");
            expect(api.Ioctl(device, 3, nullptr, &voltage) == 0 && voltage == 13500,
                   "Battery independent of ignition");
            operator_state(1, 13500);
        }
        expect(api.Connect(device, 5, 0, 500000, &ch) == j::INVALID_PROTOCOL,
               "Raw CAN not advertised");
        expect(api.Connect(device, 7, 0, 250000, &ch) == j::INVALID_BAUDRATE,
               "Wrong bitrate rejected");
        expect(api.Connect(device, 0x8005, 0, 500000, &ch) == 0, "ISO15765_PS channel");
        j::Config pin{0x8001, 0};
        j::ConfigList config{1, &pin};
        expect(api.Ioctl(ch, 1, &config, nullptr) == 0 && pin.Value == 0x060e,
               "Initial virtual pins");
        pin.Value = 0x0308;
        expect(api.Ioctl(ch, 2, &config, nullptr) == 0, "3/8 selection");
        pin.Value = 0;
        expect(api.Ioctl(ch, 1, &config, nullptr) == 0 && pin.Value == 0x0308,
               "3/8 state retained");
        pin.Value = 0x060e;
        expect(api.Ioctl(ch, 2, &config, nullptr) == 0, "6/14 virtual alias");
        pin.Value = 0x0405;
        expect(api.Ioctl(ch, 2, &config, nullptr) == j::INVALID_IOCTL_VALUE, "Other pins rejected");
        j::Config settings[] = {{0x1e, 8}, {0x1f, 0xf1}, {0x25, 10}};
        j::ConfigList cfg{3, settings};
        expect(api.Ioctl(ch, 2, &cfg, nullptr) == 0, "BS / STmin / WFT configuration");
        settings[1].Value = 0x80;
        expect(api.Ioctl(ch, 2, &cfg, nullptr) == j::INVALID_IOCTL_VALUE,
               "Reserved STmin rejected");
        settings[1].Value = 0xf1;
        expect(api.Ioctl(ch, 1, &cfg, nullptr) == 0 && settings[1].Value == 0xf1,
               "Failed config is atomic");
        j::Config unknown{0x23, 1};
        j::ConfigList u{1, &unknown};
        expect(api.Ioctl(ch, 2, &u, nullptr) == j::NOT_SUPPORTED,
               "Unimplemented TX timing is explicit");
        auto mask = msg(0x8005, 0xffffffff), pattern = msg(0x8005, 0x652),
             flow = msg(0x8005, 0x752);
        expect(api.FilterAdd(ch, 3, &mask, &pattern, &flow, &id) == 0 && id, "Exact route");
        auto filter = id;
        expect(api.FilterAdd(ch, 3, &mask, &pattern, &flow, &id) == j::EXCEEDED_LIMIT,
               "Second route rejected");
        j::Msg incoming[8]{};
        n = 1;
        expect(api.Read(ch, incoming, &n, 0) == j::BUFFER_EMPTY && n == 0,
               "Nonblocking empty read");
        n = 2;
        expect(api.Read(ch, incoming, &n, 10) == j::TIMEOUT && n == 0,
               "Read timeout preserves count");
        if (fake) {
            for (auto c : captured_cases()) {
                if (c.expected.empty())
                    continue;
                expect(api.Ioctl(ch, 8, nullptr, nullptr) == 0, "Clear fixture RX");
                for (auto m : c.expected)
                    inject(m);
                n = uint32_t(c.expected.size());
                expect(api.Read(ch, incoming, &n, 2000) == 0 && n == c.expected.size(),
                       "Fixture count");
                for (uint32_t i = 0; i < n; i++) {
                    auto &m = c.expected[i];
                    expect(incoming[i].ProtocolID == m.ProtocolID &&
                               incoming[i].RxStatus == m.RxStatus &&
                               incoming[i].Timestamp == m.Timestamp &&
                               incoming[i].DataSize == m.DataSize &&
                               incoming[i].ExtraDataIndex == m.DataSize &&
                               !std::memcmp(incoming[i].Data, m.Data, m.DataSize),
                           "Normalized captured event");
                }
                std::cout << "FIXTURE PASS " << c.name << "\n";
            }
            expect(api.Ioctl(ch, 8, nullptr, nullptr) == 0, "Clear before partial read");
            inject(msg(0x8005, 0x652, {0x62, 1}));
            n = 2;
            expect(api.Read(ch, incoming, &n, 40) == j::TIMEOUT && n == 1,
                   "Partial timeout retains delivered message");
            auto tx = msg(0x8005, 0x752, {0x22, 0xf0, 0x80});
            n = 1;
            expect(api.Write(ch, &tx, &n, 1000) == 0 && n == 1,
                   "Positive timeout waits for TX_DONE");
            n = 1;
            expect(api.Read(ch, incoming, &n, 500) == 0 && incoming[0].RxStatus == 9 &&
                       incoming[0].DataSize == 4,
                   "TX indication contains ID only");
            mode(1, 2);
            n = 1;
            expect(api.Write(ch, &tx, &n, 1000) == j::TIMEOUT && n == 0,
                   "Flow-control failure is not successful write");
            n = 1;
            expect(api.Write(ch, &tx, &n, 0) == 0 && n == 1,
                   "Asynchronous write reports queue acceptance");
            n = 1;
            expect(api.Read(ch, incoming, &n, 1000) == j::TIMEOUT && n == 0,
                   "Asynchronous transfer failure reaches the reader");
            mode(1, 0);
            expect(api.PeriodicAdd(ch, &tx, &id, 4) == j::INVALID_TIME_INTERVAL,
                   "Periodic lower bound");
            expect(api.PeriodicAdd(ch, &tx, &id, 10) == 0, "First periodic slot");
            auto first = id;
            expect(api.PeriodicAdd(ch, &tx, &id, 20) == 0, "Second periodic slot");
            auto secondPeriodic = id;
            expect(api.PeriodicAdd(ch, &tx, &id, 30) == j::EXCEEDED_LIMIT, "Periodic capacity");
            expect(api.PeriodicStop(ch, first) == 0 && api.PeriodicStop(ch, secondPeriodic) == 0,
                   "Stop periodic ownership");
        }
        expect(api.FilterStop(ch, filter) == 0, "Remove route with barrier");
        if (fake) {
            mask = msg(0x8005, 0xffffffff, {0xff});
            pattern = msg(0x8005, 0x18daf110, {0x55});
            flow = msg(0x8005, 0x18da10f1, {0xaa});
            mask.TxFlags = pattern.TxFlags = 0x180;
            flow.TxFlags = 0x1c0;
            expect(api.FilterAdd(ch, 3, &mask, &pattern, &flow, &filter) == 0,
                   "29-bit extended-address route");
            j::Config echo{3, 1};
            j::ConfigList echoConfig{1, &echo};
            expect(api.Ioctl(ch, 2, &echoConfig, nullptr) == 0, "Loopback enabled");
            auto longTx = flow;
            longTx.DataSize = 4100;
            for (unsigned i = 5; i < longTx.DataSize; i++)
                longTx.Data[i] = uint8_t(i);
            n = 1;
            expect(api.Write(ch, &longTx, &n, 1000) == 0 && n == 1,
                   "4095-byte extended TX acceptance and completion");
            n = 2;
            expect(api.Read(ch, incoming, &n, 1000) == 0 && n == 2 &&
                       incoming[0].RxStatus == 0x189 && incoming[0].DataSize == 5 &&
                       incoming[0].Data[4] == 0xaa && incoming[1].RxStatus == 0x181 &&
                       incoming[1].DataSize == 4100 &&
                       !std::memcmp(incoming[1].Data, longTx.Data, 4100),
                   "Extended TX indication and full loopback");
            echo.Value = 0;
            expect(api.Ioctl(ch, 2, &echoConfig, nullptr) == 0, "Loopback disabled");
            auto reply = pattern;
            reply.RxStatus = 0x190;
            reply.Data[5] = 0x62;
            reply.DataSize = 6;
            reply.Timestamp = 0xffffffff;
            inject(reply);
            n = 1;
            expect(api.Read(ch, incoming, &n, 1000) == 0 && incoming[0].RxStatus == 0x190 &&
                       incoming[0].Timestamp == 0xffffffff && incoming[0].Data[4] == 0x55,
                   "RX padding and 29-bit EA metadata retained");
            for (unsigned i = 0; i < 65; i++) {
                reply.Data[5] = uint8_t(i);
                inject(reply);
            }
            mode(3, 0); // All 65 packets have reached the bounded RX queue.
            std::vector<j::Msg> batchRead(64);
            n = 64;
            expect(api.Read(ch, batchRead.data(), &n, 2000) == 0 && n == 64 &&
                       batchRead[0].Data[5] == 0 && batchRead[63].Data[5] == 63,
                   "RX queue preserves first 64 entries");
            n = 1;
            expect(api.Read(ch, incoming, &n, 2000) == j::BUFFER_OVERFLOW && n == 0,
                   "RX overflow is reported");
            expect(api.Ioctl(ch, 8, nullptr, nullptr) == 0, "Clear RX overflow latch");
            mode(2, 200);
            std::vector<j::Msg> batchWrite(64, longTx);
            n = 64;
            expect(api.Write(ch, batchWrite.data(), &n, 0) == j::BUFFER_FULL && n == 63,
                   "256 KiB TX queue rejects excess without overwriting");
            expect(api.Ioctl(ch, 7, nullptr, nullptr) == 0, "Clear queued TX with barrier");
            expect(api.Ioctl(ch, 8, nullptr, nullptr) == 0, "Clear old RX indications");
            mode(2, 0);
            Sleep(50);
            n = 1;
            expect(api.Read(ch, incoming, &n, 0) == j::BUFFER_EMPTY && n == 0,
                   "Old terminal events cannot repopulate cleared RX");
            expect(api.FilterStop(ch, filter) == 0, "Remove extended route");
            char mainError[80]{}, threadError[80]{};
            expect(api.Ioctl(ch, 0x7777, nullptr, nullptr) == j::INVALID_IOCTL,
                   "Unknown IOCTL has explicit error");
            api.Error(mainError);
            std::thread other([&] {
                api.Connect(device, 5, 0, 500000, &second);
                api.Error(threadError);
            });
            other.join();
            char after[80]{};
            api.Error(after);
            expect(std::string(mainError) == after && std::string(threadError) != after,
                   "Last error is isolated per caller thread");
        }
        uint32_t readCode = 0, readCount = 1;
        std::thread waiting([&] { readCode = api.Read(ch, incoming, &readCount, 5000); });
        Sleep(100);
        auto start = now_ms();
        expect(api.Disconnect(ch) == 0, "Disconnect wakes reader");
        waiting.join();
        expect(readCode == j::INVALID_CHANNEL && readCount == 0 && now_ms() - start < 1000,
               "Blocked reader released promptly");
        uint32_t previous = ch;
        for (int i = 0; i < 124; i++) {
            expect(api.Connect(device, 0x8005, 0, 500000, &ch) == 0 && ch != previous,
                   "Sequential channel generation");
            pin.Value = 0x0308;
            expect(api.Ioctl(ch, 2, &config, nullptr) == 0, "Lifecycle fixture pins");
            expect(api.Disconnect(ch) == 0, "Lifecycle close");
            previous = ch;
        }
        std::cout << "FIXTURE PASS channel_and_pin_lifecycle\n";
        if (fake) {
            mode(0, 0);
            expect(api.Ioctl(device, 3, nullptr, &voltage) == j::DEVICE_NOT_CONNECTED,
                   "Lost USB invalidates emulated voltage");
        }
        auto closed = api.Close(device);
        expect(closed == 0 || (fake && closed == j::DEVICE_NOT_CONNECTED),
               "Close releases ownership");
        std::cout << "API CHECKS PASS " << checks << " backend="
                  << (fake ? "simulated" : "real WinUSB, no CAN messages transmitted") << "\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "API TEST FAILED after " << checks << " checks: " << e.what() << "\n";
        return 1;
    }
}
