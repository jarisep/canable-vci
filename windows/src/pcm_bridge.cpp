// SPDX-License-Identifier: Apache-2.0
// CANable-specific Mediator adaptation; all other providers retain native behavior.
// The pinned native binary stays in its original installed location.
#include "rpc.hpp"
#include <atomic>
#include <mutex>

using namespace vci;
static HMODULE self;
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static bool ready = false;
static std::atomic<bool> canable{false};
static std::atomic<uint32_t> requestId{0};
static std::mutex logMutex;
extern "C" {
void *pcm_targets[33]{};
}
enum ExportIndex {
#define PCM_ENTRY(name, index, kind) IDX_##name = index,
#include "pcm_forwarders.inc"
#undef PCM_ENTRY
};
static const char *names[] = {
#define PCM_ENTRY(name, index, kind) #name,
#include "pcm_forwarders.inc"
#undef PCM_ENTRY
};
static_assert(sizeof(names) / sizeof(*names) == 33);
static std::wstring directory() {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(self, path, MAX_PATH))
        throw std::runtime_error("Bridge module path unavailable");
    std::wstring value(path);
    return value.substr(0, value.find_last_of(L"\\/"));
}
static void log(const std::string &message) noexcept {
    try {
        std::lock_guard<std::mutex> lock(logMutex);
        auto folder = directory();
        folder = folder.substr(0, folder.find_last_of(L"\\/")); // dist
        folder = folder.substr(0, folder.find_last_of(L"\\/")); // workspace
        const auto path = folder + L"\\logs\\mediator-bridge.log";
        HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return;
        LARGE_INTEGER size{};
        if (GetFileSizeEx(file, &size) && size.QuadPart >= 1048576) {
            CloseHandle(file);
            MoveFileExW(path.c_str(), (path + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
            file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return;
        }
        const auto line = std::to_string(GetTickCount64()) +
                          " pid=" + std::to_string(GetCurrentProcessId()) + " " + message + "\r\n";
        DWORD written = 0;
        WriteFile(file, line.data(), DWORD(line.size()), &written, nullptr);
        CloseHandle(file);
    } catch (...) {
    }
}
static uint32_t __cdecl unavailable() {
    return 1; // PDU_ERR_FCT_FAILED; unknown forwarded arguments stay untouched.
}
static BOOL CALLBACK load(PINIT_ONCE, PVOID, PVOID *) noexcept {
    for (auto &entry : pcm_targets)
        entry = reinterpret_cast<void *>(unavailable);
    try {
        // The installer verifies this immutable native binary's SHA256.
        const wchar_t *path =
            L"C:\\Program Files (x86)\\Stellantis_VCIMediator\\drv\\PCMToJ2534.dll";
        HMODULE native = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!native || native == self)
            throw std::runtime_error("Native PCMToJ2534 unavailable or recursive");
        void *resolved[33]{};
        for (unsigned i = 0; i < 33; ++i) {
            resolved[i] = reinterpret_cast<void *>(GetProcAddress(native, names[i]));
            if (!resolved[i])
                throw std::runtime_error(std::string("Native export missing: ") + names[i]);
        }
        std::memcpy(pcm_targets, resolved, sizeof(resolved));
        ready = true;
        log("Native PCMToJ2534 loaded; own CANable identity enabled");
    } catch (const std::exception &e) {
        log(e.what());
    } catch (...) {
        log("Bridge initialization failed");
    }
    return TRUE;
}
extern "C" void __cdecl CanableBridgeInitialize() noexcept {
    InitOnceExecuteOnce(&once, load, nullptr, nullptr);
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH)
        self = module;
    return TRUE;
}
template <typename T> static T function(unsigned index) {
    CanableBridgeInitialize();
    return reinterpret_cast<T>(pcm_targets[index]);
}
static bool own(const char *name, const char *type = nullptr) {
    return name && _stricmp(name, "CANable ISO-TP") == 0 &&
           (!type || !*type || _stricmp(type, "PASSTHRU0404") == 0);
}
extern "C" uint32_t __cdecl PCM_SetMVCI(const char *name) {
    auto fn = function<uint32_t(__cdecl *)(const char *)>(IDX_PCM_SetMVCI);
    const auto result = fn(name);
    if (!result)
        canable = own(name);
    return result;
}
extern "C" uint32_t __cdecl PCM_SetMVCIExp(const char *type, const char *name, uint32_t id) {
    auto fn =
        function<uint32_t(__cdecl *)(const char *, const char *, uint32_t)>(IDX_PCM_SetMVCIExp);
    const auto result = fn(type, name, id);
    if (!result)
        canable = own(name, type);
    return result;
}
struct Module {
    uint32_t type, id;
    char *name, *vendor;
    uint32_t status;
};
static_assert(sizeof(Module) == 20);
extern "C" uint32_t __cdecl PCM_GetAvailableModules(Module *modules, uint32_t *count) {
    auto fn = function<uint32_t(__cdecl *)(Module *, uint32_t *)>(IDX_PCM_GetAvailableModules);
    const auto result = fn(modules, count);
    if (!result && canable && modules && count) {
        // Native 0x10054989/0x100549a5 require 255-byte name/vendor buffers.
        for (uint32_t i = 0; i < *count; ++i) {
            if (modules[i].name)
                std::memcpy(modules[i].name, "CANable ISO-TP", sizeof("CANable ISO-TP"));
        }
    }
    return result;
}
extern "C" uint32_t __cdecl PCM_ReadVoltage(uint32_t module, uint32_t type, uint32_t *mv) {
    auto native =
        function<uint32_t(__cdecl *)(uint32_t, uint32_t, uint32_t *)>(IDX_PCM_ReadVoltage);
    if (!canable || type != 3)
        return native(module, type, mv);
    if (!ready || !mv)
        return 1;
    // Validate this native module and its live USB transport first. Battery and
    // ignition remain independent; this value is only a connectivity check.
    uint32_t battery = 0;
    const auto result = native(module, 4, &battery);
    if (result)
        return result;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    try {
        wchar_t endpoint[256]{};
        const auto ini = directory() + L"\\..\\CanableJ2534.ini";
        GetPrivateProfileStringW(L"Service", L"PipeName", L"\\\\.\\pipe\\CodexCanableVci", endpoint,
                                 256, ini.c_str());
        pipe = pipe_open(endpoint);
        Writer body;
        body.u32(0); // Read-only CONTROL/status, not an ignition override.
        body.u32(0);
        Rpc request{CONTROL, ++requestId, 0, 0, body.b}, response;
        if (!rpc_send(pipe, request) || !rpc_reply(pipe, response) || response.result ||
            response.op != request.op || response.request != request.request || response.owner)
            throw std::runtime_error("Ignition state query failed");
        Reader data{response.body};
        const auto healthy = data.u32(), ignition = data.u32();
        data.u32();
        data.u32();
        data.u32();
        data.end();
        if (!healthy || (ignition != 0 && ignition != 13500))
            throw std::runtime_error("CANable transport lost or ignition state invalid");
        CloseHandle(pipe);
        pipe = INVALID_HANDLE_VALUE;
        *mv = ignition;
        log("PCM_ReadVoltage own_module=" + std::to_string(module) +
            " TypeVoltage=3 emulated_mv=" + std::to_string(ignition));
        return 0;
    } catch (...) {
        if (pipe != INVALID_HANDLE_VALUE)
            CloseHandle(pipe);
        log("CANable ignition query failed; returning error");
        return 1;
    }
}
extern "C" uint32_t __cdecl PCM_Unprepare() {
    auto fn = function<uint32_t(__cdecl *)()>(IDX_PCM_Unprepare);
    const auto result = fn();
    if (!result)
        canable = false;
    return result;
}
// Tail calls preserve the native API's stack arguments without inventing private
// signatures. Initialization runs outside DllMain and preserves x86 registers.
#define PCM_FORWARD(name, index)                                                                   \
    extern "C" __attribute__((naked)) void name() {                                                \
        __asm__ volatile("pushal\ncall _CanableBridgeInitialize\npopal\njmp *_pcm_targets+" #index \
                         "*4\n");                                                                  \
    }
#define PCM_CUSTOM(name, index)
#define PCM_ENTRY(name, index, kind) PCM_##kind(name, index)
#include "pcm_forwarders.inc"
#undef PCM_ENTRY
#undef PCM_CUSTOM
#undef PCM_FORWARD
