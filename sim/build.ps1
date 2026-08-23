# Build (and optionally run) the PC simulator.
#   .\build.ps1                        # build mid_coach
#   .\build.ps1 -Panel ent_center -Run
#   .\build.ps1 -Shot preview.bmp      # headless screenshot (home screen)
#   .\build.ps1 -Shot preview.bmp -Screen2   # ...of screen 2 instead
#   .\build.ps1 -Panel main_cabinet -Shot p.bmp -Section LIGHTS
param(
    [string]$Panel = "mid_coach",
    [switch]$Run,
    [string]$Shot = "",
    [switch]$Screen2,
    [switch]$Popup,     # with -Screen2: also open the per-pack detail popup
    # Tap a section button by name before the shot. Needed on a side-nav
    # panel, which has more sections than -Screen2/-Screen3 can name.
    [string]$Section = ""
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$gcc = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe"
if (-not (Test-Path $gcc)) {
    $cmd = Get-Command gcc -ErrorAction SilentlyContinue
    if ($cmd) { $gcc = $cmd.Source } else { throw "gcc not found — install WinLibs (winget install BrechtSanders.WinLibs.POSIX.UCRT)" }
}

# cmake + ninja come with the ESP-IDF tools if not otherwise installed.
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    . C:\esp\esp-idf\export.ps1 *> $null
}

$build = "build_$Panel"
# Note: -D args must be quoted or PowerShell passes "$Panel" literally.
cmake -G Ninja -B $build "-DPANEL=$Panel" "-DCMAKE_C_COMPILER=$gcc" -DCMAKE_BUILD_TYPE=Release .
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
cmake --build $build
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$exe = Join-Path $build "sim_$Panel.exe"
Write-Host "Built $exe"
if ($Shot -ne "") {
    if ($Section -ne "")      { & $exe --shot $Shot "section:$Section" }
    elseif ($Screen2 -and $Popup) { & $exe --shot $Shot screen2 popup }
    elseif ($Screen2)         { & $exe --shot $Shot screen2 }
    else                      { & $exe --shot $Shot }
}
elseif ($Run)    { & $exe }
