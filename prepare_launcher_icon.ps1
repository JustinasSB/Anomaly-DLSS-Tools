# v0.91.7: Extract the matching icon from the user's own AnomalyDX11AVX.exe.
# Never distribute the game's icon or require modifications to the original EXE.
# Optional override: ANOMALY_NGX_ICON_SOURCE_EXE (or ANOMALY_NGX_GAME_EXE).
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$outDir = Join-Path $root 'build_out'
$ico = Join-Path $outDir 'anomaly_ngx_launcher_icon.ico'
$res = Join-Path $outDir 'anomaly_ngx_launcher_icon.res'
if (-not (Test-Path -LiteralPath $outDir -PathType Container)) {
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
}
# Never reuse a stale icon from a different managed game installation.
if (Test-Path -LiteralPath $ico) { Remove-Item -LiteralPath $ico -Force }
if (Test-Path -LiteralPath $res) { Remove-Item -LiteralPath $res -Force }

function Test-GameExe([string]$path) {
    if (-not $path) { return $null }
    if ((Test-Path -LiteralPath $path -PathType Leaf) -and
        ([IO.Path]::GetFileName($path) -ieq 'AnomalyDX11AVX.exe')) {
        return (Resolve-Path -LiteralPath $path).ProviderPath
    }
    return $null
}
function Find-Under([string]$base) {
    if (-not $base -or -not (Test-Path -LiteralPath $base -PathType Container)) { return $null }
    foreach ($relative in @('bin\AnomalyDX11AVX.exe',
                            'Anomaly\bin\AnomalyDX11AVX.exe',
                            'AnomalyDX11AVX.exe')) {
        $found = Test-GameExe (Join-Path $base $relative)
        if ($found) { return $found }
    }
    return $null
}
function Get-MO2GamePath([string]$iniPath) {
    if (-not (Test-Path -LiteralPath $iniPath -PathType Leaf)) { return $null }
    $section = ''
    foreach ($line in [IO.File]::ReadAllLines($iniPath)) {
        if ($line -match '^\s*\[([^\]]+)\]\s*$') { $section = $Matches[1]; continue }
        if ($section -ne 'General' -or $line -notmatch '^\s*gamePath\s*=\s*(.*?)\s*$') { continue }
        $value = $Matches[1].Trim()
        if ($value.StartsWith('@ByteArray(') -and $value.EndsWith(')')) {
            $value = $value.Substring(11, $value.Length - 12)
        }
        $value = $value.Trim('"').Replace('\\', '\')
        if (-not [IO.Path]::IsPathRooted($value)) {
            $value = Join-Path (Split-Path -Parent $iniPath) $value
        }
        return $value
    }
    return $null
}

# Respect explicit overrides. An invalid override is an error: never silently
# extract an icon from an unrelated Anomaly installation.
$override = $env:ANOMALY_NGX_ICON_SOURCE_EXE
if (-not $override) { $override = $env:ANOMALY_NGX_GAME_EXE }
if ($override) {
    $gameExe = Test-GameExe $override
    if (-not $gameExe) {
        Write-Error "Icon source override must point to an existing AnomalyDX11AVX.exe: $override"
        exit 2
    }
} else {
    $gameExe = $null
    # Keep the probe bounded to ancestors of the source tree, plus their common
    # portable MO2/game layouts. Do not scan drives or choose arbitrary copies.
    $directory = [IO.DirectoryInfo]::new($root)
    for ($i = 0; $directory -and $i -lt 12; $i++) {
        $gameExe = Find-Under $directory.FullName
        if ($gameExe) { break }
        $managedGame = Get-MO2GamePath (Join-Path $directory.FullName 'ModOrganizer.ini')
        if ($managedGame) {
            $gameExe = Find-Under $managedGame
            if ($gameExe) { break }
        }
        $directory = $directory.Parent
    }
}
if (-not $gameExe) {
    Write-Warning 'AnomalyDX11AVX.exe could not be found for icon extraction. Launcher can still build, but its icon will be generic. Set ANOMALY_NGX_ICON_SOURCE_EXE to the full game EXE path and rebuild to embed the matching icon.'
    exit 0
}

try {
    Add-Type -AssemblyName System.Drawing
    $icon = [System.Drawing.Icon]::ExtractAssociatedIcon($gameExe)
    if ($null -eq $icon) { throw 'No associated executable icon was found' }
    try {
        $stream = [System.IO.File]::Create($ico)
        try { $icon.Save($stream) } finally { $stream.Dispose() }
    } finally { $icon.Dispose() }
    # The generated icon must be a proper ICO, not merely renamed PNG/BMP data.
    $bytes = [IO.File]::ReadAllBytes($ico)
    if ($bytes.Length -lt 22 -or $bytes[0] -ne 0 -or $bytes[1] -ne 0 -or
        $bytes[2] -ne 1 -or $bytes[3] -ne 0) { throw 'Extracted icon is not a valid ICO' }
    Write-Host "LAUNCHER_ICON_SOURCE: $gameExe"
} catch {
    if (Test-Path -LiteralPath $ico) { Remove-Item -LiteralPath $ico -Force }
    Write-Error "Could not extract game icon: $_"
    exit 3
}
