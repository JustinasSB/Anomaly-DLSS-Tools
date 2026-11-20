# Anomaly NGX / DLSS_Tools

Unofficial **DLSS Super Resolution and DLAA bridge** for the supported S.T.A.L.K.E.R. Anomaly multiplayer and GAMMA DirectX 11 executables. This repository contains the source for the injected D3D11/NGX bridge, its MO2-aware launcher, an optional controller, and the Mod Organizer 2 scripts/menu.

---

## DLAA Showcase — Preview the effects

**Watch DLAA ON vs. OFF differences in-game**

**[Open the interactive showcase](https://justinassb.github.io/Anomaly-DLSS-Tools/)**

---

**Version:** v0.91.7 (stable source baseline). **Platform:** Windows x64, DirectX 11, compatible NVIDIA RTX GPU and current NVIDIA driver. This is a community mod, not an NVIDIA- or game-developer-endorsed product.

> **Compatibility:** The bridge checks the game's executable SHA-256 against known profiles before installing hooks. It does **not** support arbitrary Anomaly/GAMMA builds. A different executable hash will fail closed.

## Build from source

You need **Visual Studio 2022** with *Desktop development with C++* (MSVC x64 compiler and Windows SDK), and the two dependencies below. The current build uses the NVIDIA NGX SDK directly; **NVIDIA Streamline is not required**.

| Dependency | Official source | Expected local files |
| --- | --- | --- |
| **MinHook** | [TsudaKageyu/minhook](https://github.com/TsudaKageyu/minhook) | `MinHook/include/MinHook.h`, `MinHook/src/*.c` |
| **NVIDIA DLSS / NGX SDK** | [NVIDIA/DLSS](https://github.com/NVIDIA/DLSS) | `DLSS/include/nvsdk_ngx.h`, `DLSS/include/nvsdk_ngx_helpers*.h`, `DLSS/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib` |
| **`nvngx_dlss.dll` (release runtime)** | [NVIDIA/DLSS release runtime](https://github.com/NVIDIA/DLSS/tree/main/lib/Windows_x86_64/rel) | `DLSS/lib/Windows_x86_64/rel/nvngx_dlss.dll` |

In a terminal in the repository directory, download the dependencies:

```bat
git clone https://github.com/TsudaKageyu/minhook.git MinHook
git clone https://github.com/NVIDIA/DLSS.git DLSS
```

The NVIDIA SDK also has a `dev/` DLL; **do not use or redistribute that development binary as a public release**. Use `lib/Windows_x86_64/rel/nvngx_dlss.dll`. The build links NVIDIA's `nvsdk_ngx_d.lib`, not a substitute wrapper. Follow NVIDIA's [SDK license](https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt) and redistribution/third-party-notice requirements if you publish compiled installers.

Open **x64 Native Tools Command Prompt for VS 2022** in the repo directory and run:

```bat
build.bat
```

After a successful build:

- `build_out/Anomaly (DX11-NGX).exe` — launcher.
- `build_out/anomaly_ngx_bridge_v085.dll` — injected DLSS/DLAA bridge.
- `build_out/anomaly_ngx_control_v053.exe` — optional controller.
- **`build_out/DLSS_Tools.zip`** — install-ready MO2 mod (`gamedata/` and `tools/` at ZIP root, including the NVIDIA release DLL from your local SDK).

The older `v085`/`v053` **source and DLL filenames are intentional**: they remain part of the working build/injection contract even though the release version is newer.

**Nonstandard dependency locations:** set `MINHOOK_DIR` and/or `NGX_SDK_DIR` to the corresponding full directories before `build.bat`. To supply your own authorized release feature DLL instead of the SDK's `rel/` copy, set `ANOMALY_NGX_DLSS_DLL` to its full path or place it at the repository root as `nvngx_dlss.dll`. Locally supplied DLLs and all compiled binaries are Git-ignored.

**Launcher icon:** during the build, `prepare_launcher_icon.ps1` tries to extract the icon from your local `AnomalyDX11AVX.exe` and embed it in `Anomaly (DX11-NGX).exe`. It never includes the game icon in Git. If automatic discovery fails, the launcher will build with a generic icon. To force icon extraction, set the variable `ANOMALY_NGX_ICON_SOURCE_EXE` to the full path to your installed `AnomalyDX11AVX.exe` before running `build.bat`.

## Install with Mod Organizer 2

1. Import **`build_out/DLSS_Tools.zip`** through MO2's *Install a new mod from archive* and enable the resulting **DLSS_Tools** mod.
2. In MO2's **Modify Executables**, add an executable pointing to `tools/Anomaly (DX11-NGX).exe` **inside the installed DLSS_Tools mod**.
3. The launcher discovers the game from the active portable MO2 instance's `ModOrganizer.ini` or common GAMMA layouts, then launches the original `AnomalyDX11AVX.exe` with the correct working directory. If this check fails, which can happen, you can link it explicitly or remove other executables so there's no ambiguity.

For nonstandard setups, pass the full game EXE path as the launcher's first argument.
Existing user settings are preserved at `%LOCALAPPDATA%\anomaly_ngx\settings.ini`.

4. Launch through MO2 and configure DLAA/DLSS in the game's **Video → Basic** menu.


## How it works

1. **Launcher (`anomaly_ngx_launcher_v085.cpp`)** starts the original DX11-AVX executable under MO2, waits for D3D11 initialization, and loads `anomaly_ngx_bridge_v085.dll` into that process with Windows `LoadLibraryW`.
2. **Executable verification** selects a known multiplayer or GAMMA adapter by SHA-256 before the bridge changes any engine behavior. Unknown binaries are rejected.
3. **Multiplayer renderer hooks** use [MinHook](https://github.com/TsudaKageyu/minhook) to intercept verified engine frame/composite and shader-constant paths. A fixed **8-phase Halton jitter** is injected into supported projection constants and can be synchronized with the SSFX shader jitter uniform.
4. **NGX integration** feeds the D3D11 scene color, depth and motion-vector inputs, jitter metadata and render/output dimensions to NVIDIA DLSS/DLAA, then redirects the final composite to the reconstructed output.
5. **GAMMA uses a separate adapter:** private input textures and a verified final-composite draw-time handoff, rather than multiplayer's shader-constant rewrite path.

The available DLSS presets are **J, K, L, M**. The menu offers an **SSFX Sync Halton** on/off toggle to synchronize jitters if required, to avoid runny edges.

## Limitations

- The DLL installs runtime hooks and depends on **specific game executable hashes and engine layout**. Other game builds or mod combinations may fail or crash.
- This is intended for permitted Anomaly/GAMMA configurations. It does not alter the original EXE.

## Sources, dependencies and licensing

This repository intentionally does **not** contain the game's executable/assets, NVIDIA SDK headers/import libraries/DLLs, MinHook sources or any extracted icon. Those are obtained from their respective owners under their own licenses. **Do not commit the downloaded `DLSS/` or `MinHook/` folders or `build_out/`.** If distributing a ZIP with `nvngx_dlss.dll`, check NVIDIA's distribution conditions and required notices.

**Project license:** No license is selected by this source cleanup. The repository owner should add a license for their own code before inviting reuse or contributions. Licensing your code does not relicense NVIDIA's or MinHook's components.
