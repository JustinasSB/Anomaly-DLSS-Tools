# Assemble a clean MO2 installer from tracked mod data and build_out binaries.
# NVIDIA and MinHook SDKs and all binaries remain outside Git source control.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$mod = Join-Path $root 'DLSS_Tools'
$out = Join-Path $root 'build_out'
$stage = Join-Path $out 'DLSS_Tools_stage'
$zip = Join-Path $out 'DLSS_Tools.zip'

if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'gamedata') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'tools') | Out-Null
Get-ChildItem -LiteralPath (Join-Path $mod 'gamedata') | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $stage 'gamedata') -Recurse -Force
}
$generated = @('anomaly_ngx_bridge_v085.dll', 'Anomaly (DX11-NGX).exe', 'anomaly_ngx_control_v053.exe')
foreach ($file in $generated) {
    $src = Join-Path $out $file
    if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { throw "Missing compiled file: $src" }
    Copy-Item -LiteralPath $src -Destination (Join-Path $stage 'tools') -Force
}
$ini = Join-Path (Join-Path $mod 'tools') 'anomaly_ngx_v085.ini'
if (-not (Test-Path -LiteralPath $ini -PathType Leaf)) { throw "Missing release INI: $ini" }
Copy-Item -LiteralPath $ini -Destination (Join-Path $stage 'tools') -Force

# Allow a separate, locally obtained NVIDIA release feature DLL or the SDK rel/ DLL.
# Never package the SDK's dev/ DLL (not redistributable).
$requested = $env:ANOMALY_NGX_DLSS_DLL
if ($requested) {
    if (-not (Test-Path -LiteralPath $requested -PathType Leaf)) { throw "Invalid ANOMALY_NGX_DLSS_DLL: $requested" }
    $feature = $requested
} else {
    $sdk = $env:NGX_SDK_DIR
    if (-not $sdk) { $sdk = Join-Path $root 'DLSS' }
    $candidates = @((Join-Path $root 'nvngx_dlss.dll'),
                    (Join-Path $sdk 'lib\Windows_x86_64\rel\nvngx_dlss.dll'))
    $feature = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
}
if (-not $feature) {
    throw 'Missing nvngx_dlss.dll: get the release DLL from NVIDIA/DLSS lib/Windows_x86_64/rel/ or set ANOMALY_NGX_DLSS_DLL.'
}
Copy-Item -LiteralPath $feature -Destination (Join-Path $stage 'tools\nvngx_dlss.dll') -Force

$readme = Join-Path $mod 'README.txt'
if (Test-Path -LiteralPath $readme -PathType Leaf) {
    Copy-Item -LiteralPath $readme -Destination $stage -Force
}
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
$entries = @((Join-Path $stage 'gamedata'), (Join-Path $stage 'tools'))
if (Test-Path -LiteralPath (Join-Path $stage 'README.txt')) { $entries += (Join-Path $stage 'README.txt') }
Compress-Archive -LiteralPath $entries -DestinationPath $zip -CompressionLevel Optimal -Force
if (-not (Test-Path -LiteralPath $zip -PathType Leaf)) { throw 'MO2 ZIP was not created.' }
Write-Host "MO2 package ready: $zip"
