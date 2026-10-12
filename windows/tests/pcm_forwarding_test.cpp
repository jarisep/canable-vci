// SPDX-License-Identifier: Apache-2.0
// Check export ordinals, native tail-call ABI, and non-CANable delegation.
// No module is connected and no CAN requests are sent.
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <windows.h>

int main() {
    try {
        const wchar_t *root = L"C:\\Program Files (x86)\\Stellantis_VCIMediator";
        SetCurrentDirectoryW(root);
        SetDllDirectoryW(root);
        auto native =
            LoadLibraryExW(L"C:\\Program Files (x86)\\Stellantis_VCIMediator\\drv\\PCMToJ2534.dll",
                           nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        auto bridge = LoadLibraryExW(L"C:\\codex-canable-vci\\dist\\mediator\\CanablePcmBridge.dll",
                                     nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!native || !bridge || native == bridge)
            throw std::runtime_error("Native and bridge modules must load separately");
        unsigned exports = 0;
#define PCM_ENTRY(name, index, kind)                                                               \
    if (!GetProcAddress(native, #name) || !GetProcAddress(bridge, #name) ||                        \
        GetProcAddress(bridge, #name) != GetProcAddress(bridge, MAKEINTRESOURCEA(index + 1)))      \
        throw std::runtime_error("Export or ordinal mismatch: " #name);                            \
    ++exports;
#include "../src/pcm_forwarders.inc"
#undef PCM_ENTRY
        using NoArgs = uint32_t(__cdecl *)();
        using Select = uint32_t(__cdecl *)(const char *, const char *, uint32_t);
        using Version = uint32_t(__cdecl *)(uint32_t, void *);
        using Voltage = uint32_t(__cdecl *)(uint32_t, uint32_t, uint32_t *);
        const auto nprepare = reinterpret_cast<NoArgs>(GetProcAddress(native, "PCM_Prepare"));
        const auto bprepare = reinterpret_cast<NoArgs>(GetProcAddress(bridge, "PCM_Prepare"));
        const auto nunprepare = reinterpret_cast<NoArgs>(GetProcAddress(native, "PCM_Unprepare"));
        const auto bunprepare = reinterpret_cast<NoArgs>(GetProcAddress(bridge, "PCM_Unprepare"));
        if (nprepare() || bprepare())
            throw std::runtime_error("Native/forwarded Prepare failed");
        const auto select = reinterpret_cast<Select>(GetProcAddress(bridge, "PCM_SetMVCIExp"));
        if (select("PASSTHRU0404", "Bosch - MTS6532", 0))
            throw std::runtime_error("Preserved Bosch provider selection failed");
        uint8_t nversion[0x128], bversion[0x128];
        std::memset(nversion, 0xa5, sizeof(nversion));
        std::memset(bversion, 0xa5, sizeof(bversion));
        auto nv = reinterpret_cast<Version>(GetProcAddress(native, "PCM_GetVersion"));
        auto bv = reinterpret_cast<Version>(GetProcAddress(bridge, "PCM_GetVersion"));
        const auto nr = nv(0xffffffffu, nversion), br = bv(0xffffffffu, bversion);
        if (!nr || nr != br || std::memcmp(nversion, bversion, sizeof(nversion)))
            throw std::runtime_error("Forwarded two-argument native ABI/result differs");
        auto nvoltage = reinterpret_cast<Voltage>(GetProcAddress(native, "PCM_ReadVoltage"));
        auto bvoltage = reinterpret_cast<Voltage>(GetProcAddress(bridge, "PCM_ReadVoltage"));
        uint32_t nmv = 0xa5a5, bmv = 0xa5a5;
        const auto ne = nvoltage(0xffffffffu, 3, &nmv), be = bvoltage(0xffffffffu, 3, &bmv);
        if (!ne || ne != be || nmv != bmv)
            throw std::runtime_error("Non-CANable ignition behavior differs from native");
        if (bunprepare() || nunprepare())
            throw std::runtime_error("Native/bridge Unprepare failed");
        FreeLibrary(bridge);
        FreeLibrary(native);
        std::cout
            << "PASS: " << exports
            << " native export ordinals, forwarded x86 ABI and non-CANable delegation; no CAN TX\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
