@echo off
setlocal EnableExtensions
cd /d "%~dp0"

rem Public source build: SDK sources and binaries are acquired by each developer.
if not defined MINHOOK_DIR set "MINHOOK_DIR=%CD%\MinHook"
if not defined NGX_SDK_DIR set "NGX_SDK_DIR=%CD%\DLSS"

if not exist "%MINHOOK_DIR%\include\MinHook.h" (
    echo ERROR: MinHook not found. Clone https://github.com/TsudaKageyu/minhook into MinHook, or set MINHOOK_DIR.
    exit /b 2
)
if not exist "%MINHOOK_DIR%\src\hde\hde64.c" (
    echo ERROR: Incomplete MinHook source tree at "%MINHOOK_DIR%".
    exit /b 2
)
if not exist "%NGX_SDK_DIR%\include\nvsdk_ngx.h" (
    echo ERROR: NVIDIA DLSS SDK headers not found. Clone https://github.com/NVIDIA/DLSS into DLSS, or set NGX_SDK_DIR.
    exit /b 3
)
if not exist "%NGX_SDK_DIR%\include\nvsdk_ngx_helpers.h" (
    echo ERROR: NVIDIA DLSS SDK helper header not found.
    exit /b 3
)
if not exist "%NGX_SDK_DIR%\include\nvsdk_ngx_helpers_d3d.h" (
    echo ERROR: NVIDIA DLSS SDK D3D helper header not found.
    exit /b 3
)
set "NGX_LIB_DIR=%NGX_SDK_DIR%\lib\Windows_x86_64\x64"
if not exist "%NGX_LIB_DIR%\nvsdk_ngx_d.lib" set "NGX_LIB_DIR=%NGX_SDK_DIR%\lib\Windows_x86_64"
if not exist "%NGX_LIB_DIR%\nvsdk_ngx_d.lib" set "NGX_LIB_DIR=%NGX_SDK_DIR%\lib\x64"
if not exist "%NGX_LIB_DIR%\nvsdk_ngx_d.lib" (
    echo ERROR: Official nvsdk_ngx_d.lib not found under "%NGX_SDK_DIR%\lib".
    exit /b 4
)
where cl.exe >nul 2>&1
if errorlevel 1 (
    echo ERROR: Run this from the Visual Studio x64 Native Tools Command Prompt.
    exit /b 5
)
where powershell.exe >nul 2>&1
if errorlevel 1 (
    echo ERROR: Windows PowerShell is needed for icon extraction and packaging.
    exit /b 5
)
if not exist "DLSS_Tools\gamedata\scripts\anomaly_ngx_pause_auto.script" (
    echo ERROR: Missing multiplayer pause/bootstrap Lua script.
    exit /b 9
)
if not exist "DLSS_Tools\gamedata\scripts\anomaly_ngx_gamma_bootstrap.script" (
    echo ERROR: Missing GAMMA Lua bootstrap script.
    exit /b 10
)
findstr /L /C:"mp_bootstrap_v080." "DLSS_Tools\gamedata\scripts\anomaly_ngx_pause_auto.script" >nul
if errorlevel 1 (
    echo ERROR: Multiplayer Lua bootstrap prefix missing.
    exit /b 11
)
findstr /L /C:"mp_bootstrap_on_actor_update" "DLSS_Tools\gamedata\scripts\anomaly_ngx_pause_auto.script" >nul
if errorlevel 1 (
    echo ERROR: Multiplayer Lua bootstrap callback missing.
    exit /b 11
)
findstr /L /C:"MP_BOOTSTRAP_DISPATCH" "DLSS_Tools\gamedata\scripts\anomaly_ngx_pause_auto.script" >nul
if errorlevel 1 (
    echo ERROR: Multiplayer Lua bootstrap restart dispatch missing.
    exit /b 11
)

if not exist "build_out" mkdir "build_out"
echo Building Anomaly NGX v0.91.7 (x64)...
pushd "build_out"
cl /nologo /W3 /O2 /EHsc /std:c++17 /MD /LD ^
 /I "%MINHOOK_DIR%\include" /I "%MINHOOK_DIR%\src" /I "%NGX_SDK_DIR%\include" ^
 "..\anomaly_ngx_bridge_v085.cpp" ^
 "%MINHOOK_DIR%\src\buffer.c" "%MINHOOK_DIR%\src\hook.c" ^
 "%MINHOOK_DIR%\src\trampoline.c" "%MINHOOK_DIR%\src\hde\hde64.c" ^
 /link /OUT:"anomaly_ngx_bridge_v085.dll" /IMPLIB:"anomaly_ngx_bridge_v085.lib" ^
 /LIBPATH:"%NGX_LIB_DIR%" nvsdk_ngx_d.lib ^
 d3d11.lib d3dcompiler.lib dxgi.lib dxguid.lib advapi32.lib user32.lib version.lib /DEFAULTLIB:User32.lib
if errorlevel 1 (
    popd
    echo ERROR: Bridge DLL compilation failed.
    exit /b 6
)
popd

rem Extract an icon locally from AnomalyDX11AVX.exe; do not distribute its artwork.
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "prepare_launcher_icon.ps1"
if errorlevel 1 (
    echo ERROR: Icon extraction failed. Check ANOMALY_NGX_ICON_SOURCE_EXE.
    exit /b 15
)
set "LAUNCHER_ICON_RES="
if exist "build_out\anomaly_ngx_launcher_icon.ico" (
    where rc.exe >nul 2>&1
    if errorlevel 1 (
        echo ERROR: Windows SDK resource compiler rc.exe is missing.
        exit /b 16
    )
    rc.exe /nologo /fo "build_out\anomaly_ngx_launcher_icon.res" "launcher_icon.rc"
    if errorlevel 1 (
        echo ERROR: Launcher icon resource compilation failed.
        exit /b 17
    )
    set "LAUNCHER_ICON_RES=anomaly_ngx_launcher_icon.res"
)
pushd "build_out"
cl /nologo /W3 /O2 /EHsc /std:c++17 /MD ^
 "..\anomaly_ngx_launcher_v085.cpp" %LAUNCHER_ICON_RES% ^
 /link /SUBSYSTEM:WINDOWS /OUT:"Anomaly (DX11-NGX).exe" user32.lib shell32.lib
if errorlevel 1 (
    popd
    echo ERROR: Launcher compilation failed.
    exit /b 7
)
cl /nologo /W3 /O2 /EHsc /std:c++17 /MD ^
 "..\anomaly_ngx_control_v053.cpp" ^
 /link /OUT:"anomaly_ngx_control_v053.exe" user32.lib
if errorlevel 1 (
    popd
    echo ERROR: External controller compilation failed.
    exit /b 8
)
popd

rem Build an MO2 ZIP using only tracked mod data and freshly compiled outputs.
rem Never write generated DLLs/EXEs/backup configs into the source tree.
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "package_DLSS_Tools.ps1"
if errorlevel 1 (
    echo ERROR: MO2 installer packaging failed.
    exit /b 14
)
echo SUCCESS: build_out\DLSS_Tools.zip
exit /b 0
