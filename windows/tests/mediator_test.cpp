// SPDX-License-Identifier: Apache-2.0
// Calls the installed PCMToJ2534 wrapper without starting a vehicle session.
// Signatures and inline version-buffer layout were verified from the pinned DLL.
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <windows.h>
struct Module {
    uint32_t type, id;
    char *name;
    char *vendor;
    uint32_t status;
};
static_assert(sizeof(Module) == 20);
int main(int argc, char **argv) {
    const bool ignitionOff = argc == 2 && std::string(argv[1]) == "--ignition-off";
    const bool nativeProbe = argc == 2 && std::string(argv[1]) == "--native-probe";
    bool connected = false;
    uint32_t moduleId = 0;
    uint32_t(__cdecl * disconnect)(uint32_t) = nullptr;
    HMODULE dll = nullptr;
    try {
        const wchar_t *root = L"C:\\Program Files (x86)\\Stellantis_VCIMediator";
        SetCurrentDirectoryW(root);
        SetDllDirectoryW(root);
        dll = LoadLibraryExW(
            nativeProbe ? L"C:\\Program Files (x86)\\Stellantis_VCIMediator\\drv\\PCMToJ2534.dll"
                        : L"C:\\codex-canable-vci\\dist\\mediator\\CanablePcmBridge.dll",
            nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!dll)
            throw std::runtime_error("PCMToJ2534 load failed: " + std::to_string(GetLastError()));
#define FN(name, type)                                                                             \
    auto name = reinterpret_cast<type>(GetProcAddress(dll, #name));                                \
    if (!name)                                                                                     \
    throw std::runtime_error("Missing " #name)
        FN(PCM_SetMVCI, uint32_t(__cdecl *)(const char *));
        FN(PCM_Prepare, uint32_t(__cdecl *)());
        FN(PCM_Unprepare, uint32_t(__cdecl *)());
        FN(PCM_GetAvailableModules, uint32_t(__cdecl *)(Module *, uint32_t *));
        FN(PCM_ConnectModule, uint32_t(__cdecl *)(uint32_t));
        FN(PCM_DisconnectModule, uint32_t(__cdecl *)(uint32_t));
        FN(PCM_GetVersion, uint32_t(__cdecl *)(uint32_t, void *));
        FN(PCM_ReadVoltage, uint32_t(__cdecl *)(uint32_t, uint32_t, uint32_t *));
        disconnect = PCM_DisconnectModule;
#undef FN
        auto result = PCM_SetMVCI("CANable ISO-TP");
        std::cout << "PCM_SetMVCI=" << result << std::endl;
        if (result)
            throw std::runtime_error("Provider selection failed");
        result = PCM_Prepare();
        std::cout << "PCM_Prepare=" << result << std::endl;
        if (result)
            throw std::runtime_error("Wrapper prepare failed");
        Module modules[16]{};
        char names[16][256]{}, vendors[16][256]{};
        for (unsigned i = 0; i < 16; i++) {
            modules[i].name = names[i];
            modules[i].vendor = vendors[i];
        }
        uint32_t count = 16;
        result = PCM_GetAvailableModules(modules, &count);
        std::cout << "PCM_GetAvailableModules=" << result << " count=" << count << std::endl;
        if (result || !count || count > 16)
            throw std::runtime_error("No CANable module was identified");
        for (uint32_t i = 0; i < count; i++)
            std::cout << "MODULE id=" << modules[i].id << " status=" << modules[i].status
                      << " type=" << modules[i].type << " name=" << names[i]
                      << " vendor=" << vendors[i] << std::endl;
        if (!nativeProbe &&
            (std::strcmp(names[0], "CANable ISO-TP") || std::strcmp(vendors[0], "CANable project")))
            throw std::runtime_error("Unexpected CANable module name or vendor");
        moduleId = modules[0].id;
        uint32_t unavailableVoltage = 0;
        if (PCM_ReadVoltage(moduleId, 3, &unavailableVoltage) == 0)
            throw std::runtime_error("Ignition succeeded before module connection");
        result = PCM_ConnectModule(moduleId);
        std::cout << "PCM_ConnectModule=" << result << std::endl;
        if (result)
            throw std::runtime_error("Module connect failed");
        connected = true;
        // The verified wrapper zeroes and fills a 0x128-byte inline version record.
        alignas(4) uint8_t version[0x128]{};
        result = PCM_GetVersion(moduleId, version);
        std::cout << "PCM_GetVersion=" << result << std::endl;
        if (result)
            throw std::runtime_error("Mediator version failed");
        for (uint32_t type : {4u, 3u}) {
            uint32_t mv = 0;
            result = PCM_ReadVoltage(moduleId, type, &mv);
            std::cout << "PCM_ReadVoltage TypeVoltage=" << type << " result=" << result
                      << " millivolts=" << mv << std::endl;
            if (nativeProbe && type == 3) {
                if (result != 0x63)
                    throw std::runtime_error("Native vendor restriction differs");
                continue;
            }
            const auto expected = type == 3 && ignitionOff ? 0u : 13500u;
            if (result || mv != expected)
                throw std::runtime_error("Mediator voltage path failed");
        }
        result = PCM_DisconnectModule(moduleId);
        connected = false;
        std::cout << "PCM_DisconnectModule=" << result << std::endl;
        if (result)
            throw std::runtime_error("Module disconnect failed");
        if (PCM_ReadVoltage(moduleId, 3, &unavailableVoltage) == 0)
            throw std::runtime_error("Ignition succeeded after module disconnection");
        PCM_Unprepare();
        FreeLibrary(dll);
        std::cout << (nativeProbe ? "NATIVE OWN-PROVIDER DISCOVERY AND VENDOR RESTRICTION "
                                  : "MEDIATOR IDENTIFICATION AND IGNITION ")
                  << (ignitionOff ? "OFF" : "ON") << " PASS; no logical CAN link or CAN messages\n";
        return 0;
    } catch (const std::exception &e) {
        if (connected && disconnect)
            disconnect(moduleId);
        std::cerr << "MEDIATOR TEST FAILED: " << e.what() << "\n";
        if (dll)
            FreeLibrary(dll);
        return 1;
    }
}
