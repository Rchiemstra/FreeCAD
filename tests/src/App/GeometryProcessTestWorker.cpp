// SPDX-License-Identifier: LGPL-2.1-or-later

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#ifdef _WIN32
# include <windows.h>
#else
# include <csignal>
# include <sys/types.h>
# include <unistd.h>
#endif

using namespace std::chrono_literals;

namespace
{

std::string environment(const char* name)
{
    const char* value = std::getenv(name);
    return value ? value : "";
}

bool waitForGate()
{
    const std::filesystem::path gate = environment("FREECAD_GEOMETRY_START_GATE");
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code error;
        if (std::filesystem::is_regular_file(gate, error)) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

void heartbeat()
{
    std::ofstream stream(environment("FREECAD_GEOMETRY_HEARTBEAT"),
                         std::ios::out | std::ios::trunc);
    stream << "test-heartbeat\n";
}

[[noreturn]] void hang()
{
    for (;;) {
        std::this_thread::sleep_for(1s);
    }
}

#ifdef _WIN32
unsigned long spawnChild()
{
    wchar_t executable[MAX_PATH] {};
    const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return 0;
    }
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --child";
    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process {};
    if (!CreateProcessW(nullptr,
                        command.data(),
                        nullptr,
                        nullptr,
                        FALSE,
                        CREATE_NO_WINDOW,
                        nullptr,
                        nullptr,
                        &startup,
                        &process)) {
        return 0;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return process.dwProcessId;
}
#else
unsigned long spawnChild()
{
    const pid_t child = ::fork();
    if (child == 0) {
        hang();
    }
    return child > 0 ? static_cast<unsigned long>(child) : 0;
}
#endif

}  // namespace

int main(const int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--child") {
        hang();
    }
    if (!waitForGate()) {
        return 11;
    }
    const std::string operation = environment("FREECAD_GEOMETRY_OPERATION");
    if (operation == "Test.Crash") {
        std::abort();
    }
    if (operation == "Test.OutOfMemory") {
        return 86;
    }
    if (operation == "Test.NoHeartbeat") {
        hang();
    }
    heartbeat();
    if (operation == "Test.CooperativeCancel") {
        const std::filesystem::path cancel = environment("FREECAD_GEOMETRY_CANCEL");
        for (;;) {
            std::error_code error;
            if (std::filesystem::is_regular_file(cancel, error)) {
                return 85;
            }
            std::this_thread::sleep_for(5ms);
        }
    }
    if (operation == "Test.ChildHang") {
        const auto child = spawnChild();
        std::ofstream pid(environment("FREECAD_GEOMETRY_RESULT") + ".child-pid",
                          std::ios::out | std::ios::trunc);
        pid << child << '\n';
        pid.close();
        if (child == 0) {
            return 15;
        }
        hang();
    }
    if (operation == "Test.Hang") {
        hang();
    }
    return 13;
}
