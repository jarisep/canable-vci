// SPDX-License-Identifier: Apache-2.0
#include "engine.hpp"
#include "async_log.hpp"
#include <fstream>
#include <iostream>
#include <sddl.h>
#include <set>
#include <sstream>
using namespace vci;
static std::atomic<bool> stopping{false};
static std::mutex clientsMutex;
static std::condition_variable clientsCV;
static std::set<HANDLE> clients;
static SERVICE_STATUS_HANDLE scmHandle;
static SERVICE_STATUS scmStatus{};
static std::wstring base, ini;
static std::string logPath;
static AsyncLog *asyncLog = nullptr;
static void log(const std::string &text) {
    if (asyncLog) asyncLog->write(now_ms(), text);
}
static std::wstring setting(const wchar_t *section, const wchar_t *key, const wchar_t *fallback) {
    wchar_t b[512];
    GetPrivateProfileStringW(section, key, fallback, b, 512, ini.c_str());
    return b;
}
static std::wstring own_sid() {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        throw std::runtime_error("Token query failed");
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    Bytes b(size);
    if (!GetTokenInformation(token, TokenUser, b.data(), size, &size)) {
        CloseHandle(token);
        throw std::runtime_error("Token user query failed");
    }
    LPWSTR sid = nullptr;
    ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(b.data())->User.Sid, &sid);
    std::wstring result = sid ? sid : L"";
    if (sid)
        LocalFree(sid);
    CloseHandle(token);
    if (result.empty())
        throw std::runtime_error("User SID lookup failed");
    return result;
}
static void stop() {
    stopping = true;
    std::lock_guard<std::mutex> l(clientsMutex);
    for (auto h : clients)
        CancelIoEx(h, nullptr);
}
static BOOL WINAPI console(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        stop();
        return TRUE;
    }
    return FALSE;
}
static void client_thread(HANDLE h, Engine &engine) {
    uint64_t owned = 0;
    DWORD process = 0;
    GetNamedPipeClientProcessId(h, &process);
    try {
        for (;;) {
            Rpc req;
            if (!rpc_receive(h, req, owned ? INFINITE : 5000) || stopping)
                break;
            if (req.result || !req.request)
                break;
            Rpc reply;
            try {
                reply = engine.call(req, process);
            } catch (const Error &e) {
                reply = {req.op, req.request, e.code, req.owner, {}};
                if (req.op == READ || req.op == WRITE) {
                    Writer w;
                    w.u32(0);
                    reply.body = w.b;
                } else
                    reply.body.assign(e.what(), e.what() + std::strlen(e.what()));
                log("RPC error pid=" + std::to_string(process) + " op=" + std::to_string(req.op) +
                    " code=" + std::to_string(e.code) + " " + e.what());
            } catch (const std::exception &e) {
                reply = {req.op, req.request, j::FAILED, req.owner, {}};
                if (req.op == READ || req.op == WRITE) {
                    Writer w;
                    w.u32(0);
                    reply.body = w.b;
                } else
                    reply.body.assign(e.what(), e.what() + std::strlen(e.what()));
                log(e.what());
            }
            if (req.op == OPEN && reply.result == 0)
                owned = reply.owner;
            log("RPC pid=" + std::to_string(process) + " op=" + std::to_string(req.op) + " bytes=" +
                std::to_string(req.body.size()) + " result=" + std::to_string(reply.result));
            if (!rpc_send(h, reply))
                break;
            if (req.op == CLOSE || !owned) {
                if (req.op != OPEN) {
                    uint8_t ack = 0;
                    pipe_io(h, &ack, 1, false, 2000);
                }
                break;
            }
        }
    } catch (const std::exception &e) {
        log(e.what());
    }
    if (owned)
        engine.release(owned);
    {
        std::lock_guard<std::mutex> l(clientsMutex);
        clients.erase(h);
        DisconnectNamedPipe(h);
        CloseHandle(h);
        clientsCV.notify_all();
    }
}
static int serve(bool simulation) {
    auto pipe = simulation ? L"\\\\.\\pipe\\CodexCanableVciTest"
                           : setting(L"Service", L"PipeName", L"\\\\.\\pipe\\CodexCanableVci");
    auto sid = setting(L"Service", L"AllowedUserSid", own_sid().c_str());
    auto acl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + sid + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1,
                                                              &descriptor, nullptr))
        throw std::runtime_error("Invalid pipe access control");
    SECURITY_ATTRIBUTES sa{sizeof(sa), descriptor, FALSE};
    std::unique_ptr<Client> backend;
    if (simulation)
        backend = std::make_unique<FakeClient>();
    else
        backend =
            std::make_unique<UsbClient>(setting(L"Device", L"Serial", L""));
    Engine engine(std::move(backend), simulation, [](const std::string &s) { log(s); });
    log(simulation ? "SIMULATION service started; no USB device access" : "WinUSB service started; async_log=256_records shared_fc_firmware_required=ciso-0.2.1");
    bool first = true;
    while (!stopping) {
        HANDLE pipeHandle = CreateNamedPipeW(
            pipe.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 32, 65536,
            65536, 0, &sa);
        if (pipeHandle == INVALID_HANDLE_VALUE) {
            log("CreateNamedPipe failed: " + std::to_string(GetLastError()));
            if (first) {
                LocalFree(descriptor);
                return 1;
            }
            Sleep(100);
            continue;
        }
        first = false;
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL connected = ConnectNamedPipe(pipeHandle, &ov);
        auto error = connected ? ERROR_SUCCESS : GetLastError();
        if (error == ERROR_IO_PENDING) {
            while (!stopping && WaitForSingleObject(ov.hEvent, 100) == WAIT_TIMEOUT) {
            }
            DWORD got = 0;
            if (stopping)
                CancelIoEx(pipeHandle, &ov);
            connected = GetOverlappedResult(pipeHandle, &ov, &got, TRUE);
        } else
            connected = error == ERROR_PIPE_CONNECTED || connected;
        CloseHandle(ov.hEvent);
        if (!connected || stopping) {
            CloseHandle(pipeHandle);
            continue;
        }
        {
            std::lock_guard<std::mutex> l(clientsMutex);
            clients.insert(pipeHandle);
        }
        std::thread(client_thread, pipeHandle, std::ref(engine)).detach();
    }
    LocalFree(descriptor);
    engine.shutdown();
    {
        std::unique_lock<std::mutex> l(clientsMutex);
        for (auto h : clients)
            CancelIoEx(h, nullptr);
        clientsCV.wait(l, [] { return clients.empty(); });
    }
    log("Service stopped");
    return 0;
}
static DWORD WINAPI service_control(DWORD control, DWORD, void *, void *) {
    if (control == SERVICE_CONTROL_STOP) {
        scmStatus.dwCurrentState = SERVICE_STOP_PENDING;
        scmStatus.dwWaitHint = 10000;
        SetServiceStatus(scmHandle, &scmStatus);
        stop();
    }
    return NO_ERROR;
}
static void WINAPI service_main(DWORD, LPWSTR *) {
    scmHandle = RegisterServiceCtrlHandlerExW(L"CodexCanableVci", service_control, nullptr);
    if (!scmHandle)
        return;
    scmStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    scmStatus.dwCurrentState = SERVICE_RUNNING;
    scmStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    SetServiceStatus(scmHandle, &scmStatus);
    int result = 1;
    try {
        result = serve(false);
    } catch (const std::exception &e) {
        log(e.what());
    }
    scmStatus.dwCurrentState = SERVICE_STOPPED;
    scmStatus.dwWin32ExitCode = result ? ERROR_SERVICE_SPECIFIC_ERROR : 0;
    scmStatus.dwServiceSpecificExitCode = result;
    SetServiceStatus(scmHandle, &scmStatus);
}
int main(int argc, char **argv) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    base = path;
    base = base.substr(0, base.find_last_of(L"\\/"));
    ini = base + L"\\CanableVciService.ini";
    auto root = base.substr(0, base.find_last_of(L"\\/"));
    auto lp = root + L"\\logs\\service.log";
    logPath = std::string(lp.begin(), lp.end());
    RotatingLogFile logFile(logPath);
    AsyncLog logger([&](const std::string &line) { logFile(line); }, 256, [] {
        OutputDebugStringA("CANable service log write failed; check log directory/disk.\n");
    });
    asyncLog = &logger;
    SetConsoleCtrlHandler(console, TRUE);
    try {
        if (argc > 1 && std::string(argv[1]) == "--service") {
            SERVICE_TABLE_ENTRYW entries[] = {
                {const_cast<LPWSTR>(L"CodexCanableVci"), service_main}, {nullptr, nullptr}};
            if (!StartServiceCtrlDispatcherW(entries))
                return int(GetLastError());
            return 0;
        }
        if (argc > 1 &&
            (std::string(argv[1]) == "--status" || std::string(argv[1]) == "--set-ignition")) {
            Writer w;
            bool set = std::string(argv[1]) == "--set-ignition";
            if (set &&
                (argc != 3 || (std::string(argv[2]) != "on" && std::string(argv[2]) != "off")))
                throw std::runtime_error("Use --set-ignition on|off");
            w.u32(set ? 1 : 0);
            w.u32(set && std::string(argv[2]) == "on" ? 13500 : 0);
            auto h = pipe_open(setting(L"Service", L"PipeName", L"\\\\.\\pipe\\CodexCanableVci"));
            Rpc req{CONTROL, 1, 0, 0, w.b}, reply;
            if (!rpc_send(h, req) || !rpc_reply(h, reply))
                throw std::runtime_error("Control request failed");
            CloseHandle(h);
            if (reply.result)
                throw std::runtime_error("Control action rejected");
            Reader r{reply.body};
            std::cout << "usb_owned=" << r.u32() << " ignition_mv=" << r.u32()
                      << " vbatt_mv=" << r.u32() << " channel=" << r.u32()
                      << " virtual_pins=" << r.u32() << " physical_pins=3/8\n";
            return 0;
        }
        return serve(argc > 1 && std::string(argv[1]) == "--simulate");
    } catch (const std::exception &e) {
        log(e.what());
        std::cerr << e.what() << "\n";
        return 1;
    }
}
