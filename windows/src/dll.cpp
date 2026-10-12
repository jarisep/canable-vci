// SPDX-License-Identifier: Apache-2.0
#include "rpc.hpp"
#include <atomic>
#include <functional>
#include <mutex>
using namespace vci;
static HMODULE module;
static std::mutex lifecycle;
static HANDLE control = INVALID_HANDLE_VALUE;
static std::atomic<uint64_t> token{0};
static std::atomic<uint32_t> requests{0};
static uint32_t device = 0;
static thread_local std::string lastError = "No error";
static std::wstring pipeName;
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH)
        module = h;
    return TRUE;
}
static void memory(const void *p, size_t n, bool write = false) {
    if (!p)
        throw Error(j::NULL_PARAMETER, "Null parameter");
    auto at = reinterpret_cast<uintptr_t>(p);
    if (n > UINTPTR_MAX - at)
        throw Error(j::NULL_PARAMETER, "Invalid buffer range");
    auto end = at + n;
    while (at < end) {
        MEMORY_BASIC_INFORMATION m{};
        if (!VirtualQuery(reinterpret_cast<const void *>(at), &m, sizeof(m)) ||
            m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            throw Error(j::NULL_PARAMETER, "Inaccessible caller buffer");
        auto access = m.Protect & 0xff;
        if (write && access != PAGE_READWRITE && access != PAGE_WRITECOPY &&
            access != PAGE_EXECUTE_READWRITE && access != PAGE_EXECUTE_WRITECOPY)
            throw Error(j::NULL_PARAMETER, "Caller buffer is not writable");
        uintptr_t next = reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize;
        if (next <= at)
            throw Error(j::NULL_PARAMETER, "Invalid memory region");
        at = std::min<uintptr_t>(next, end);
    }
}
static uint32_t guard(const std::function<void()> &f) {
    try {
        f();
        lastError = "No error";
        return 0;
    } catch (const Error &e) {
        lastError = e.what();
        return e.code;
    } catch (const std::exception &e) {
        lastError = e.what();
        return j::FAILED;
    } catch (...) {
        lastError = "Unhandled internal error";
        return j::FAILED;
    }
}
static std::wstring settings() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(module, path, MAX_PATH))
        throw Error(j::FAILED, "Cannot locate DLL settings");
    std::wstring s(path);
    return s.substr(0, s.find_last_of(L"\\/")) + L"\\CanableJ2534.ini";
}
static Rpc exchange(HANDLE h, Rpc req, uint32_t timeout = 5000) {
    if (!rpc_send(h, req))
        throw Error(j::DEVICE_NOT_CONNECTED, "Service pipe write failed; no replay");
    Rpc reply;
    if (!rpc_reply(h, reply, timeout) || reply.request != req.request || reply.op != req.op ||
        (req.op != OPEN && reply.owner != req.owner))
        throw Error(j::DEVICE_NOT_CONNECTED, "Service pipe response is invalid or lost");
    return reply;
}
static Rpc call(uint16_t op, const Bytes &body, uint32_t timeout = 5000, bool partial = false) {
    uint64_t owner;
    std::wstring endpoint;
    {
        std::lock_guard<std::mutex> l(lifecycle);
        owner = token.load();
        endpoint = pipeName;
    }
    if (!owner)
        throw Error(j::INVALID_DEVICE, "PassThruOpen is required");
    HANDLE h = pipe_open(endpoint);
    Rpc reply;
    try {
        reply = exchange(h, {op, ++requests, 0, owner, body}, timeout);
    } catch (...) {
        CloseHandle(h);
        throw;
    }
    CloseHandle(h);
    if (reply.result && !partial)
        throw Error(reply.result, reply.body.empty()
                                      ? "Service operation failed"
                                      : std::string(reply.body.begin(), reply.body.end()));
    return reply;
}
static void count_ptr(uint32_t *n) {
    memory(n, 4, true);
    if (!*n || *n > 64)
        throw Error(j::EXCEEDED_LIMIT, "Message count must be 1..64");
}
extern "C" uint32_t WINAPI PassThruOpen(void *name, uint32_t *id) {
    return guard([&] {
        memory(id, 4, true);
        *id = 0;
        if (name) {
            memory(name, 1);
            if (*static_cast<char *>(name))
                throw Error(j::NOT_SUPPORTED, "Named device selection is unavailable");
        }
        std::lock_guard<std::mutex> l(lifecycle);
        if (token)
            throw Error(j::DEVICE_IN_USE, "DLL already owns a device");
        wchar_t value[256];
        auto ini = settings();
        GetPrivateProfileStringW(L"Service", L"PipeName", L"\\\\.\\pipe\\CodexCanableVci", value,
                                 256, ini.c_str());
        pipeName = value;
        control = pipe_open(pipeName);
        try {
            auto reply = exchange(control, {OPEN, ++requests, 0, 0, {}});
            if (reply.result)
                throw Error(reply.result, std::string(reply.body.begin(), reply.body.end()));
            Reader r{reply.body};
            device = r.u32();
            r.u32();
            r.end();
            if (!reply.owner || !device)
                throw Error(j::FAILED, "Invalid Open response");
            token = reply.owner;
            *id = device;
        } catch (...) {
            CloseHandle(control);
            control = INVALID_HANDLE_VALUE;
            throw;
        }
    });
}
extern "C" uint32_t WINAPI PassThruClose(uint32_t id) {
    return guard([&] {
        std::lock_guard<std::mutex> l(lifecycle);
        if (!token || id != device)
            throw Error(j::INVALID_DEVICE, "Invalid device handle");
        Writer w;
        w.u32(id);
        auto owner = token.exchange(0);
        Rpc reply;
        try {
            reply = exchange(control, {CLOSE, ++requests, 0, owner, w.b});
        } catch (...) {
            CloseHandle(control);
            control = INVALID_HANDLE_VALUE;
            device = 0;
            throw;
        }
        CloseHandle(control);
        control = INVALID_HANDLE_VALUE;
        device = 0;
        if (reply.result)
            throw Error(reply.result, std::string(reply.body.begin(), reply.body.end()));
    });
}
extern "C" uint32_t WINAPI PassThruConnect(uint32_t id, uint32_t protocol, uint32_t flags,
                                           uint32_t rate, uint32_t *channel) {
    return guard([&] {
        memory(channel, 4, true);
        *channel = 0;
        Writer w;
        w.u32(id);
        w.u32(protocol);
        w.u32(flags);
        w.u32(rate);
        auto reply = call(CONNECT, w.b);
        Reader r{reply.body};
        *channel = r.u32();
        r.end();
    });
}
extern "C" uint32_t WINAPI PassThruDisconnect(uint32_t channel) {
    return guard([&] {
        Writer w;
        w.u32(channel);
        call(DISCONNECT, w.b);
    });
}
extern "C" uint32_t WINAPI PassThruReadMsgs(uint32_t channel, j::Msg *messages, uint32_t *count,
                                            uint32_t timeout) {
    return guard([&] {
        count_ptr(count);
        auto wanted = *count;
        *count = 0;
        memory(messages, size_t(wanted) * sizeof(*messages), true);
        Writer w;
        w.u32(channel);
        w.u32(wanted);
        w.u32(timeout);
        auto reply = call(READ, w.b, std::min(timeout, 60000u) + 5000, true);
        Reader r{reply.body};
        auto found = r.u32();
        if (found > wanted)
            throw Error(j::FAILED, "Read count exceeds caller capacity");
        for (uint32_t n = 0; n < found; n++)
            messages[n] = r.msg();
        r.end();
        *count = found;
        if (reply.result)
            throw Error(reply.result, "ReadMsgs returned " + std::to_string(reply.result));
    });
}
extern "C" uint32_t WINAPI PassThruWriteMsgs(uint32_t channel, j::Msg *messages, uint32_t *count,
                                             uint32_t timeout) {
    return guard([&] {
        count_ptr(count);
        auto wanted = *count;
        *count = 0;
        memory(messages, size_t(wanted) * sizeof(*messages));
        Writer w;
        w.u32(channel);
        w.u32(wanted);
        w.u32(timeout);
        for (uint32_t n = 0; n < wanted; n++) {
            if (messages[n].DataSize > 4128)
                throw Error(j::INVALID_MSG, "DataSize exceeds ABI array");
            w.msg(messages[n]);
        }
        auto reply = call(WRITE, w.b, std::min(timeout, 60000u) + 5000, true);
        Reader r{reply.body};
        *count = r.u32();
        r.end();
        if (*count > wanted)
            throw Error(j::FAILED, "Write count exceeds requested count");
        if (reply.result)
            throw Error(reply.result, "WriteMsgs returned " + std::to_string(reply.result));
    });
}
extern "C" uint32_t WINAPI PassThruStartPeriodicMsg(uint32_t channel, j::Msg *message, uint32_t *id,
                                                    uint32_t interval) {
    return guard([&] {
        memory(message, sizeof(*message));
        memory(id, 4, true);
        *id = 0;
        Writer w;
        w.u32(channel);
        w.u32(interval);
        w.msg(*message);
        auto reply = call(PERIODIC_ADD, w.b);
        Reader r{reply.body};
        *id = r.u32();
        r.end();
    });
}
extern "C" uint32_t WINAPI PassThruStopPeriodicMsg(uint32_t channel, uint32_t id) {
    return guard([&] {
        Writer w;
        w.u32(channel);
        w.u32(id);
        call(PERIODIC_STOP, w.b);
    });
}
extern "C" uint32_t WINAPI PassThruStartMsgFilter(uint32_t channel, uint32_t type, j::Msg *mask,
                                                  j::Msg *pattern, j::Msg *flow, uint32_t *id) {
    return guard([&] {
        memory(mask, sizeof(*mask));
        memory(pattern, sizeof(*pattern));
        memory(flow, sizeof(*flow));
        memory(id, 4, true);
        *id = 0;
        Writer w;
        w.u32(channel);
        w.u32(type);
        w.msg(*mask);
        w.msg(*pattern);
        w.msg(*flow);
        auto reply = call(FILTER_ADD, w.b);
        Reader r{reply.body};
        *id = r.u32();
        r.end();
    });
}
extern "C" uint32_t WINAPI PassThruStopMsgFilter(uint32_t channel, uint32_t id) {
    return guard([&] {
        Writer w;
        w.u32(channel);
        w.u32(id);
        call(FILTER_STOP, w.b);
    });
}
extern "C" uint32_t WINAPI PassThruSetProgrammingVoltage(uint32_t device, uint32_t pin,
                                                         uint32_t voltage) {
    return guard([&] {
        Writer w;
        w.u32(device);
        w.u32(pin);
        w.u32(voltage);
        call(PROGRAM, w.b);
    });
}
extern "C" uint32_t WINAPI PassThruReadVersion(uint32_t device, char *fw, char *dll, char *api) {
    return guard([&] {
        for (auto p : {fw, dll, api})
            memory(p, 80, true);
        Writer w;
        w.u32(device);
        auto reply = call(VERSION, w.b);
        Reader r{reply.body};
        for (auto p : {fw, dll, api}) {
            auto n = r.u32();
            if (n > 79)
                throw Error(j::FAILED, "Version string exceeds 79 bytes");
            auto s = r.raw(n);
            std::memcpy(p, s.data(), n);
            p[n] = 0;
        }
        r.end();
    });
}
extern "C" uint32_t WINAPI PassThruGetLastError(char *output) {
    if (!output)
        return j::NULL_PARAMETER;
    try {
        memory(output, 80, true);
        auto n = std::min<size_t>(79, lastError.size());
        std::memcpy(output, lastError.data(), n);
        output[n] = 0;
        return 0;
    } catch (...) {
        return j::NULL_PARAMETER;
    }
}
extern "C" uint32_t WINAPI PassThruIoctl(uint32_t id, uint32_t ioctl, void *input, void *output) {
    return guard([&] {
        Writer w;
        w.u32(id);
        try {
            Writer trace;
            trace.u32(id);
            trace.u32(ioctl);
            trace.u32(input != nullptr);
            trace.u32(output != nullptr);
            call(TRACE, trace.b);
        } catch (...) {
        }
        if (ioctl == 1 || ioctl == 2) {
            memory(input, sizeof(j::ConfigList));
            auto list = static_cast<j::ConfigList *>(input);
            if (list->NumOfParams > 64)
                throw Error(j::EXCEEDED_LIMIT, "Configuration count exceeds 64");
            if (list->NumOfParams)
                memory(list->ConfigPtr, size_t(list->NumOfParams) * sizeof(j::Config), ioctl == 1);
            w.u32(ioctl == 2);
            w.u32(list->NumOfParams);
            for (uint32_t n = 0; n < list->NumOfParams; n++) {
                w.u32(list->ConfigPtr[n].Parameter);
                w.u32(list->ConfigPtr[n].Value);
            }
            auto reply = call(CONFIG, w.b);
            if (ioctl == 1) {
                Reader r{reply.body};
                auto n = r.u32();
                if (n != list->NumOfParams)
                    throw Error(j::FAILED, "Configuration result count differs");
                for (uint32_t i = 0; i < n; i++) {
                    auto param = r.u32(), value = r.u32();
                    if (param != list->ConfigPtr[i].Parameter)
                        throw Error(j::FAILED, "Configuration result parameter differs");
                    list->ConfigPtr[i].Value = value;
                }
                r.end();
            }
        } else if (ioctl == 3 || ioctl == 0x800e || ioctl == 0x0e) {
            if (input)
                throw Error(j::INVALID_IOCTL_VALUE, "Voltage query requires NULL input");
            memory(output, 4, true);
            w.u32(ioctl);
            auto reply = call(VOLTAGE, w.b);
            Reader r{reply.body};
            *static_cast<uint32_t *>(output) = r.u32();
            r.end();
        } else if (ioctl >= 7 && ioctl <= 10) {
            if (input || output)
                throw Error(j::INVALID_IOCTL_VALUE, "Clear requires NULL input/output");
            w.u32(1u << (ioctl - 7));
            call(CLEAR, w.b);
        } else if (ioctl == 0x800c) {
            if (input)
                throw Error(j::INVALID_IOCTL_VALUE, "Device info requires NULL input");
            memory(output, sizeof(j::ParamList));
            auto list = static_cast<j::ParamList *>(output);
            if (!list->NumOfParams || list->NumOfParams > 64)
                throw Error(j::EXCEEDED_LIMIT, "Device info count must be 1..64");
            memory(list->ParamPtr, size_t(list->NumOfParams) * sizeof(j::Param), true);
            for (uint32_t n = 0; n < list->NumOfParams; n++)
                if (list->ParamPtr[n].Parameter != 1)
                    throw Error(j::NOT_SUPPORTED, "Only observed serial Parameter=1 is supported");
            auto reply = call(INFO, w.b);
            Reader r{reply.body};
            auto serial = r.u32();
            r.end();
            for (uint32_t n = 0; n < list->NumOfParams; n++) {
                list->ParamPtr[n].Value = serial;
                list->ParamPtr[n].Supported = 1;
            }
        } else
            throw Error(j::INVALID_IOCTL, "Unsupported IOCTL " + std::to_string(ioctl));
    });
}
