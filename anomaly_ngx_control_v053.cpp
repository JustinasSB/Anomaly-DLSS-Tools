// Anomaly NGX Bridge v0.3.2: settings-file controller, mirrors Lua menu Apply.
// No process injection or hotkeys. Can change settings while game runs.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <stdlib.h> // MSVC _countof macro
#include <cstdio>
#include <cwchar>

static bool SettingsPath(wchar_t (&out)[MAX_PATH + 128]) {
    DWORD n = GetEnvironmentVariableW(L"ANOMALY_NGX_CONFIG", out, static_cast<DWORD>(_countof(out)));
    if (n && n < _countof(out)) return true;
    n = GetEnvironmentVariableW(L"XRRAZOM_DLAA_CONFIG", out, static_cast<DWORD>(_countof(out)));
    if (n && n < _countof(out)) return true;
    wchar_t dir[MAX_PATH]{};
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    wchar_t subdir[MAX_PATH + 64]{};
    swprintf_s(subdir, L"%ls\\anomaly_ngx", dir);
    CreateDirectoryW(subdir, nullptr);
    swprintf_s(out, L"%ls\\settings.ini", subdir);
    wchar_t previous[MAX_PATH + 128]{};
    swprintf_s(previous, L"%ls\\xrrazom_dlaa\\settings.ini", dir);
    CopyFileW(previous, out, TRUE); // Fail if a new config already exists.

    return true;
}
int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 3) {
        fwprintf(stderr, L"Usage: anomaly_ngx_control_v053.exe status | on | off | mode dlaa|quality|balanced|performance|ultra_performance | preset j|k|l|m | jitter 0..125 | sharpness 0..100 | ssfx off|on\n");
        return 2;
    }
    wchar_t path[MAX_PATH + 128]{};
    if (!SettingsPath(path)) { fwprintf(stderr,L"Could not determine config path.\n"); return 3; }
    const wchar_t* key=nullptr; const wchar_t* value=nullptr;
    if (_wcsicmp(argv[1],L"on")==0) {key=L"enabled";value=L"1";}
    else if (_wcsicmp(argv[1],L"off")==0) {key=L"enabled";value=L"0";}
    else if (_wcsicmp(argv[1],L"mode")==0 && argc==3) {
        if (_wcsicmp(argv[2],L"dlaa") && _wcsicmp(argv[2],L"quality") &&
            _wcsicmp(argv[2],L"balanced") && _wcsicmp(argv[2],L"performance") &&
            _wcsicmp(argv[2],L"ultra_performance")) {
            fwprintf(stderr,L"Choose dlaa, quality, balanced, performance or ultra_performance\n");return 2;
        }
        key=L"upscaler_mode";value=argv[2];
    }
    else if (_wcsicmp(argv[1],L"preset")==0 && argc==3) {
        if (_wcsicmp(argv[2],L"j")&&_wcsicmp(argv[2],L"k")&&
            _wcsicmp(argv[2],L"l")&&_wcsicmp(argv[2],L"m")) {fwprintf(stderr,L"Bad preset\n");return 2;}
        key=L"preset";value=argv[2];
    } else if ((_wcsicmp(argv[1],L"jitter")==0 || _wcsicmp(argv[1],L"sharpness")==0) && argc==3) {
        const bool isJitter = _wcsicmp(argv[1],L"jitter")==0;
        wchar_t* end=nullptr;
        const long n=wcstol(argv[2],&end,10);
        if (!end || *end != L'\0' || n<0 || n>(isJitter?125:100)) {
            fwprintf(stderr,L"Please enter a whole number in the allowed range.\n");return 2;
        }
        key=isJitter?L"jitter_percent":L"sharpness_percent";
        value=argv[2];
    } else if (_wcsicmp(argv[1],L"ssfx")==0 && argc==3) {
        key=L"sss_jitter_sync";
        if (_wcsicmp(argv[2],L"off")==0) value=L"0";
        else if (_wcsicmp(argv[2],L"on")==0) value=L"1";
        else { fwprintf(stderr,L"Choose ssfx off or on\n"); return 2; }
    } else if (_wcsicmp(argv[1],L"status")!=0) {
        fwprintf(stderr,L"Unknown command.\n");return 2;
    }
    if (key && !WritePrivateProfileStringW(L"DLAA",key,value,path)) {
        fwprintf(stderr,L"Could not write %ls (Windows error %lu)\n",path,GetLastError());return 4;
    }
    wchar_t mode[32]{};
    GetPrivateProfileStringW(L"DLAA",L"upscaler_mode",L"dlaa",mode,_countof(mode),path);
    wchar_t preset[32]{};
    GetPrivateProfileStringW(L"DLAA",L"preset",L"k",preset,_countof(preset),path);
    wprintf(L"file=%ls\nNGX=%ls requested_mode=%ls preset=%ls jitter_percent=%u sharpness_percent=%u sss_jitter_sync=%u\n",path,
        GetPrivateProfileIntW(L"DLAA",L"enabled",1,path)?L"ON":L"OFF",mode,preset,
        GetPrivateProfileIntW(L"DLAA",L"jitter_percent",100,path),
        GetPrivateProfileIntW(L"DLAA",L"sharpness_percent",0,path),
        GetPrivateProfileIntW(L"DLAA",L"sss_jitter_sync",1,path));
    wprintf(L"Changes are applied on the render thread (~0.5 seconds); file reads can run on worker.\n");
    return 0;
}
