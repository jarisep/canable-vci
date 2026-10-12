// SPDX-License-Identifier: Apache-2.0
// Read-only discovery through the installed VCIManager, not the PSA management UI.
// The supported native x86 ABI is pinned; vendor binaries are not distributed.
// No logical CAN links or diagnostic requests are created.
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>

static_assert(sizeof(void *) == 4, "VCIManager requires an x86 caller");

int main() {
    HMODULE dll = nullptr;
    void(__cdecl * unprepare)() = nullptr;
    bool prepared = false;
    try {
        const wchar_t *root = L"C:\\Program Files (x86)\\Stellantis_VCIMediator";
        SetCurrentDirectoryW(root);
        SetDllDirectoryW(root);
        dll =
            LoadLibraryExW(L"C:\\Program Files (x86)\\Stellantis_VCIMediator\\drv\\VCIManager.dll",
                           nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!dll)
            throw std::runtime_error("VCIManager load failed: " + std::to_string(GetLastError()));
#define FN(name, type)                                                                             \
    auto name = reinterpret_cast<type>(GetProcAddress(dll, #name));                                \
    if (!name)                                                                                     \
    throw std::runtime_error("Missing " #name)
        FN(VCIM_Prepare, void(__cdecl *)());
        FN(VCIM_Unprepare, void(__cdecl *)());
        FN(VCIM_GetMVCIList, uint32_t(__cdecl *)(const void *, uint32_t));
        FN(VCIM_GetCmdStatus, uint32_t(__cdecl *)(uint32_t, uint32_t *, uint32_t *, uint32_t *));
        FN(VCIM_GetCmdResult, uint32_t(__cdecl *)(uint32_t, char *, uint32_t));
        FN(VCIM_GetMVCIDescription, uint32_t(__cdecl *)(uint32_t));
#undef FN
        unprepare = VCIM_Unprepare;
        VCIM_Prepare();
        prepared = true;
        auto collect = [&](uint32_t handle) {
            if (!handle)
                throw std::runtime_error("Invalid command handle");
            const auto deadline = GetTickCount64() + 15000;
            uint32_t state = 1, size = 0, progress = 0, result = 0;
            do {
                result = VCIM_GetCmdStatus(handle, &state, &size, &progress);
                if (result || state != 1)
                    break;
                Sleep(100);
            } while (GetTickCount64() < deadline);
            std::cout << "STATUS result=" << result << " state=" << state << " size=" << size
                      << " progress=" << progress << std::endl;
            if (result || state != 2 || !size || size > 65536)
                throw std::runtime_error("Discovery command did not finish successfully");
            std::vector<char> xml(size + 1, 0);
            result = VCIM_GetCmdResult(handle, xml.data(), size);
            if (result)
                throw std::runtime_error("Cannot retrieve discovery result");
            return std::string(xml.data());
        };
        // A null options array and zero count are explicitly accepted by 0x100169b0.
        const auto list = collect(VCIM_GetMVCIList(nullptr, 0));
        std::cout << "DISCOVERY_XML\n" << list << std::endl;
        const auto name = list.find("Name=\"CANable ISO-TP\"");
        if (name == std::string::npos)
            throw std::runtime_error("Own CANable provider missing from discovery");
        const auto element = list.rfind("<MVCI ", name);
        const auto end = list.find("/>", name);
        if (element == std::string::npos || end == std::string::npos)
            throw std::runtime_error("Invalid provider XML element");
        const auto provider = list.substr(element, end + 2 - element);
        if (provider.find("Type=\"PASSTHRU0404\"") == std::string::npos)
            throw std::runtime_error("Provider has unexpected type");
        if (provider.find("Vendor=\"CANable project\"") == std::string::npos ||
            provider.find("Model=\"CANable ISO-TP\"") == std::string::npos)
            throw std::runtime_error("CANable discovery identity differs");
        const auto indexPos = provider.find("Index=\"");
        if (indexPos == std::string::npos)
            throw std::runtime_error("Provider index missing");
        const auto index = std::stoul(provider.substr(indexPos + 7));
        const auto description = collect(VCIM_GetMVCIDescription(static_cast<uint32_t>(index)));
        std::cout << "DESCRIPTION_XML\n" << description << std::endl;
        if (description.find("SerialNumber=\"") == std::string::npos ||
            description.find("FwVersion=\"V0.2.") == std::string::npos)
            throw std::runtime_error("Discovery description did not identify the real CANable");
        VCIM_Unprepare();
        prepared = false;
        FreeLibrary(dll);
        std::cout << "PASS: VCIManager discovers and describes the real CANable provider; no CAN TX"
                  << std::endl;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << std::endl;
        if (prepared && unprepare)
            unprepare();
        if (dll)
            FreeLibrary(dll);
        return 1;
    }
}
