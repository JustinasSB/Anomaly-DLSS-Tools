// Anomaly NGX v0.91.7 — readiness-gated MO2 launch wrapper.
// Starts the original EXE as an MO2-managed child and loads the self-contained
// DLAA bridge via standard LoadLibraryW. Does not replace system/game DLLs.
// For permitted offline/private instances only. No protection bypass.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <TlHelp32.h>
#include <string>
#include <cstdint>
#include <vector>
#include <cstdio>
#include <cwchar>
#include <shellapi.h> // CommandLineToArgvW for Windows-subsystem launcher

namespace {
std::wstring Dirname(const std::wstring& path) {
    const auto pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : path.substr(0, pos);
}
std::wstring Basename(const std::wstring& path) {
    const auto pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}
std::wstring Quote(const std::wstring& path) {
    std::wstring s = L"\"";
    for (wchar_t c : path) { if (c == L'"') s += L"\\"; s += c; }
    s += L"\"";
    return s;
}
// GUI subsystem: never allocate a console and never write a launcher log.
// Only actionable failures produce a message box; successful launches are silent.
void Report(const wchar_t* stage, DWORD winerr = 0) {
    wchar_t message[768]{};
    if (winerr)
        swprintf_s(message, L"%ls\n\nWindows error: %lu", stage, winerr);
    else
        swprintf_s(message, L"%ls", stage);
    MessageBoxW(nullptr, message, L"Anomaly (DX11-NGX) - launch error", MB_OK | MB_ICONERROR);
}
bool FileExists(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// v0.91.6: The launcher is installed under the MO2 mod's tools directory.
// MO2 stores its managed game directory in the instance's ModOrganizer.ini,
// but it does not pass that directory as an argument to arbitrary tools.
// Discover the game automatically; never require MO2's optional Start In field.
std::wstring JoinPath(const std::wstring& base, const wchar_t* child) {
    if (base.empty()) return std::wstring(child);
    return base + ((base.back() == L'\\' || base.back() == L'/') ? L"" : L"\\") + child;
}
std::wstring AbsolutePath(const std::wstring& path) {
    if (path.empty()) return L"";
    std::vector<wchar_t> buffer(32768);
    const DWORD count = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()),
                                         buffer.data(), nullptr);
    if (!count || count >= buffer.size()) return L"";
    return std::wstring(buffer.data(), count);
}
std::wstring CurrentDir() {
    std::vector<wchar_t> buffer(32768);
    const DWORD count = GetCurrentDirectoryW(static_cast<DWORD>(buffer.size()), buffer.data());
    return (count && count < buffer.size()) ? std::wstring(buffer.data(), count) : L"";
}
std::wstring StripQtByteArray(std::wstring value) {
    // QSettings commonly writes [General] gamePath=@ByteArray(C:\\Game\\Anomaly).
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return L"";
    const auto last = value.find_last_not_of(L" \t\r\n");
    value = value.substr(first, last - first + 1);
    const std::wstring prefix = L"@ByteArray(";
    if (value.compare(0, prefix.size(), prefix) == 0 && value.back() == L')')
        value = value.substr(prefix.size(), value.size() - prefix.size() - 1);
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"')
        value = value.substr(1, value.size() - 2);
    // Decode QSettings' doubled backslashes; leave ordinary Windows paths alone.
    std::wstring result;
    result.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == L'\\' && i + 1 < value.size() && value[i + 1] == L'\\') ++i;
        result.push_back(value[i]);
    }
    return result;
}
std::wstring FindAnomalyExeUnder(const std::wstring& base) {
    if (base.empty()) return L"";
    // MO2 may manage the Anomaly directory, the enclosing GAMMA directory,
    // or bin itself. Never silently select a different renderer executable.
    const wchar_t* const candidates[] = {
        L"bin\\AnomalyDX11AVX.exe", L"Anomaly\\bin\\AnomalyDX11AVX.exe",
        L"AnomalyDX11AVX.exe"};
    for (const wchar_t* relative : candidates) {
        const std::wstring candidate = AbsolutePath(JoinPath(base, relative));
        if (FileExists(candidate)) return candidate;
    }
    return L"";
}
std::wstring FindFromMO2Config(const std::wstring& launcherPath) {
    // Portable and GAMMA MO2 instances often keep ModOrganizer.ini above mods/.
    std::wstring directory = Dirname(launcherPath);
    for (int i = 0; i < 12 && !directory.empty(); ++i) {
        const std::wstring ini = JoinPath(directory, L"ModOrganizer.ini");
        if (FileExists(ini)) {
            wchar_t gamePath[32768]{};
            const DWORD length = GetPrivateProfileStringW(L"General", L"gamePath", L"",
                                  gamePath, static_cast<DWORD>(sizeof(gamePath)/sizeof(wchar_t)), ini.c_str());
            if (length && length < 32767) {
                const std::wstring path = StripQtByteArray(gamePath);
                if (path.empty()) continue;
                // Relative managed-game paths are relative to the MO2 instance,
                // not to the launcher's current working directory.
                const bool absolute = (path.size() >= 3 && path[1] == L':') ||
                                      (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\');
                const std::wstring managedBase = absolute ? path : JoinPath(directory, path.c_str());
                if (const std::wstring found = FindAnomalyExeUnder(managedBase); !found.empty())
                    return found;
            }
        }
        const std::wstring parent = Dirname(directory);
        if (parent == directory || parent == L".") break;
        directory = parent;
    }
    return L"";
}
std::wstring AutoDiscoverGame(const std::wstring& launcherPath) {
    // Explicit environment override for nonstandard MO2 locations. An explicit
    // command-line game path continues to take precedence over this variable.
    wchar_t overrideExe[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"ANOMALY_NGX_GAME_EXE", overrideExe, 32768);
    if (length && length < 32768) {
        const std::wstring game = AbsolutePath(overrideExe);
        return FileExists(game) ? game : L""; // invalid override: fail, never guess
    }
    if (const std::wstring game = FindFromMO2Config(launcherPath); !game.empty())
        return game;
    // Fall back to the working directory and nearby known layouts, not a global
    // disk/registry scan that could accidentally select the wrong Anomaly copy.
    if (const std::wstring game = FindAnomalyExeUnder(CurrentDir()); !game.empty())
        return game;
    std::wstring directory = Dirname(launcherPath);
    for (int i = 0; i < 12 && !directory.empty(); ++i) {
        if (const std::wstring game = FindAnomalyExeUnder(directory); !game.empty())
            return game;
        const std::wstring parent = Dirname(directory);
        if (parent == directory || parent == L".") break;
        directory = parent;
    }
    return L"";
}
// Do not assume kernelbase/kernel32 is mapped at the same base across processes.
LPTHREAD_START_ROUTINE FindRemoteLoadLibrary(DWORD pid) {
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (!kernel) return nullptr;
    const FARPROC local = GetProcAddress(kernel, "LoadLibraryW");
    if (!local) return nullptr;
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(local), &owner) || !owner)
        return nullptr;
    wchar_t localPath[MAX_PATH]{};
    if (!GetModuleFileNameW(owner, localPath, MAX_PATH)) return nullptr;
    const std::wstring ownerName = Basename(localPath);
    const std::uintptr_t offset = reinterpret_cast<std::uintptr_t>(local) -
                                  reinterpret_cast<std::uintptr_t>(owner);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return nullptr;
    MODULEENTRY32W e{}; e.dwSize = sizeof(e);
    LPTHREAD_START_ROUTINE result = nullptr;
    if (Module32FirstW(snap, &e)) {
        do {
            if (_wcsicmp(e.szModule, ownerName.c_str()) == 0 && offset < e.modBaseSize) {
                result = reinterpret_cast<LPTHREAD_START_ROUTINE>(
                    reinterpret_cast<std::uintptr_t>(e.modBaseAddr) + offset);
                break;
            }
        } while (Module32NextW(snap, &e));
    }
    CloseHandle(snap);
    return result;
}
bool HasLoadedModule(DWORD pid, const wchar_t* moduleName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W e{}; e.dwSize = sizeof(e);
    bool found = false;
    if (Module32FirstW(snap, &e)) {
        do {
            if (_wcsicmp(e.szModule, moduleName) == 0) { found = true; break; }
        } while (Module32NextW(snap, &e));
    }
    CloseHandle(snap);
    return found;
}

bool Inject(DWORD pid, const std::wstring& dllPath) {
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                    PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE,
                    FALSE, pid);
    if (!process) { Report(L"OpenProcess failed", GetLastError()); return false; }
    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { Report(L"VirtualAllocEx failed", GetLastError()); CloseHandle(process); return false; }
    SIZE_T written = 0;
    bool success = false;
    if (WriteProcessMemory(process, remote, dllPath.c_str(), bytes, &written) && written == bytes) {
        LPTHREAD_START_ROUTINE loadLibrary = nullptr;
        for (int i = 0; i < 80 && !loadLibrary; ++i) {
            loadLibrary = FindRemoteLoadLibrary(pid);
            if (!loadLibrary) Sleep(100);
        }
        if (loadLibrary) {
            HANDLE worker = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
            if (worker) {
                const DWORD wait = WaitForSingleObject(worker, 15000);
                if (wait == WAIT_OBJECT_0) {
                    DWORD result = 0;
                    if (GetExitCodeThread(worker, &result) && result != 0) {
                        // DWORD is a truncated x64 module handle; nonzero is enough here.
                        success = true;
                        // Successful injection is silent.
                    } else Report(L"LoadLibraryW returned zero; check DLL dependencies", GetLastError());
                } else Report(L"Remote loader timed out", GetLastError());
                CloseHandle(worker);
            } else Report(L"CreateRemoteThread failed", GetLastError());
        } else Report(L"Could not find the remote LoadLibraryW address", GetLastError());
    } else Report(L"WriteProcessMemory failed", GetLastError());
    // Free remote string when worker completed; a timed-out thread might still
    // be reading it, so deliberately keep memory allocated in that case.
    if (success) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    return success;
}
}
static int LauncherMain(int argc, wchar_t* argv[]) {
    wchar_t launcherPath[MAX_PATH]{};
    const DWORD launcherLen = GetModuleFileNameW(nullptr, launcherPath, MAX_PATH);
    if (!launcherLen || launcherLen >= MAX_PATH) {
        Report(L"Could not determine the launcher installation directory");
        return 3;
    }
    const std::wstring dll = Dirname(launcherPath) + L"\\anomaly_ngx_bridge_v085.dll";
    if (!FileExists(dll)) { Report((L"Missing bridge DLL: " + dll).c_str()); return 4; }

    // Backwards compatible: the first argument, if provided, is the game EXE.
    // With no argument, resolve the currently managed Anomaly installation.
    const std::wstring game = (argc >= 2) ? AbsolutePath(argv[1])
                                           : AutoDiscoverGame(launcherPath);
    if (!FileExists(game)) {
        if (argc >= 2)
            Report((L"The game executable argument does not exist: " + std::wstring(argv[1])).c_str());
        else
            Report(L"Could not locate AnomalyDX11AVX.exe automatically.\n\n"
                   L"In MO2, select this launcher and leave Arguments and Start In blank. "
                   L"If your MO2 instance uses a nonstandard mod directory, set "
                   L"ANOMALY_NGX_GAME_EXE or pass the full game EXE path as the first argument.");
        return 5;
    }
    std::wstring command = Quote(game);
    for (int i = 2; i < argc; ++i) command += L" " + Quote(argv[i]);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    // Create shared, writable settings once. The child Lua and injected DLL read
    // exactly the same path from the inherited environment variable.
    wchar_t local[MAX_PATH]{};
    DWORD localLen = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!localLen || localLen >= MAX_PATH) {
        Report(L"LOCALAPPDATA unavailable; cannot establish Lua/DLL configuration channel");
        return 9;
    }
    const std::wstring configDir = std::wstring(local) + L"\\anomaly_ngx";
    if (!CreateDirectoryW(configDir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        Report(L"Configuration directory creation failed", GetLastError());
        return 10;
    }
    const std::wstring configFile = configDir + L"\\settings.ini";
    // One-time migration. Preserve old file and NEVER overwrite a new setting.
    // This retains existing mode/preset and SSS sync preferences. Legacy halton_phases is ignored by the DLL.
    const std::wstring previousConfig = std::wstring(local) + L"\\xrrazom_dlaa\\settings.ini";
    if (!FileExists(configFile) && FileExists(previousConfig)) {
        CopyFileW(previousConfig.c_str(), configFile.c_str(), TRUE);
    }
    HANDLE config = CreateFileW(configFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                 nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (config != INVALID_HANDLE_VALUE) {
        const char defaults[] = "[DLAA]\r\nenabled=1\r\npreset=k\r\nsss_jitter_sync=1\r\n";
        DWORD bytes = 0;
        WriteFile(config, defaults, static_cast<DWORD>(sizeof(defaults)-1), &bytes, nullptr);
        CloseHandle(config);
    } else if (GetLastError() != ERROR_FILE_EXISTS) {
        Report(L"Failed to initialize settings file", GetLastError());
        return 11;
    }
    if (!SetEnvironmentVariableW(L"ANOMALY_NGX_CONFIG", configFile.c_str())) {
        Report(L"Failed to pass settings path to game", GetLastError());
        return 12;
    }
    // Legacy environment key only for interoperability with an old Lua menu.
    SetEnvironmentVariableW(L"XRRAZOM_DLAA_CONFIG", configFile.c_str());
    // v078: fresh process-scoped token. Only a matching GAMMA DLL request can
    // trigger the Lua one-shot restart; stale requests cannot affect MP or
    // the next game launch. This never changes MP's rendering behavior.
    wchar_t bootstrapToken[64]{};
    swprintf_s(bootstrapToken,L"%lu_%lu",GetCurrentProcessId(),GetTickCount());
    if(!SetEnvironmentVariableW(L"ANOMALY_NGX_BOOTSTRAP_V078_TOKEN",bootstrapToken)) {
        Report(L"Failed to establish per-launch bootstrap token",GetLastError());
        return 13;
    }

    const std::wstring gameDir = Dirname(game);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(game.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                        0, nullptr, gameDir.c_str(), &startup, &child)) {
        Report(L"CreateProcessW failed", GetLastError());
        return 6;
    }
    CloseHandle(child.hThread);
    // Optional early injection: capture renderer target allocations before they
    // happen. This is separate from the unchanged DLAA/rendering hooks.
    const std::wstring startupConfig = Dirname(launcherPath) + L"\\anomaly_ngx_v085.ini";
    const bool earlyRequested = GetPrivateProfileIntW(L"SR_ALLOCATION_AUDIT", L"early_injection", 0,
                                                      startupConfig.c_str()) != 0;
    bool earlyLoaded = false;
    if (earlyRequested) {
        for (unsigned attempt = 0; attempt < 800; ++attempt) {
            if (WaitForSingleObject(child.hProcess, 0) != WAIT_TIMEOUT) break;
            if (HasLoadedModule(child.dwProcessId, L"kernel32.dll")) {
                earlyLoaded = Inject(child.dwProcessId, dll);
                // Fall back to normal D3D11-gated injection silently.
                break;
            }
            Sleep(10);
        }
    }
    if (earlyLoaded) {
        WaitForSingleObject(child.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(child.hProcess, &exitCode);
        CloseHandle(child.hProcess);
        return static_cast<int>(exitCode);
    }
    // The old launcher injected as soon as the game looked input-idle. In MO2
    // this often precedes graphics initialization, so gate on d3d11.dll.
    bool d3dReady = false;
    for (unsigned attempt = 0; attempt < 600; ++attempt) {
        if (WaitForSingleObject(child.hProcess, 0) != WAIT_TIMEOUT) {
            Report(L"Game exited before D3D11 loaded");
            CloseHandle(child.hProcess);
            return 8;
        }
        if (HasLoadedModule(child.dwProcessId, L"d3d11.dll")) {
            d3dReady = true;
            break;
        }
        Sleep(200);
    }
    // Keep the original startup delay and D3D11 readiness gate unchanged.
    (void)d3dReady;
    Sleep(1000);
    const bool loaded = Inject(child.dwProcessId, dll);
    // Inject() already displays the specific failure once; no duplicate dialog.
    WaitForSingleObject(child.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(child.hProcess, &exitCode);
    CloseHandle(child.hProcess);
    return loaded ? static_cast<int>(exitCode) : 7;
}

// /SUBSYSTEM:WINDOWS makes MO2 launch silent: no command prompt window,
// no success/confirmation lines and no console inheritance from the launcher.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        Report(L"Could not parse launcher command line", GetLastError());
        return 2;
    }
    const int rc = LauncherMain(argc, argv);
    LocalFree(argv);
    return rc;
}
