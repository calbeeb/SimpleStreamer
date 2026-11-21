param(
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$devkitpro = $env:DEVKITPRO
if (-not $devkitpro -or $devkitpro -eq "/opt/devkitpro" -or -not (Test-Path $devkitpro)) {
    if (Test-Path "C:/devkitPro") {
        $devkitpro = "C:/devkitPro"
    }
}
$devkitpro = $devkitpro.Replace("\", "/")
$env:DEVKITPRO = $devkitpro

$prefix = "$devkitpro/devkitA64/bin/aarch64-none-elf-"
$gcc = "${prefix}gcc.exe"
$elf2nso = "$devkitpro/tools/bin/elf2nso.exe"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptDir

if ($Clean) {
    Write-Host "Cleaning..."
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue build, stream-module.nso, build/stream-module.elf
    Write-Host "Clean complete."
    exit 0
}

if (-not (Test-Path "build")) {
    New-Item -ItemType Directory -Path "build" | Out-Null
}

$cflags = @(
    "-mcpu=cortex-a78c+crypto", "-mtp=soft", "-fPIE",
    "-std=gnu11", "-O2", "-Wall", "-Wextra",
    "-ffunction-sections", "-fdata-sections",
    "-D__SWITCH__", "-D__SWITCH2__",
    "-I$devkitpro/portlibs/switch2/include",
    "-I$devkitpro/libounce/include",
    "-Isource"
)

$sources = @("main.c", "capture.c", "network.c", "audio.c")
$objs = @()

foreach ($src in $sources) {
    $obj = "build/" + [System.IO.Path]::ChangeExtension($src, ".o")
    $objs += $obj
    Write-Host "Compiling source/$src..."
    & $gcc $cflags -c "source/$src" -o $obj
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Failed to compile $src"
        exit 1
    }
}

$ldflags = @(
    "-mcpu=cortex-a78c+crypto", "-mtp=soft", "-fPIE",
    "-specs=$devkitpro/libounce/switch.specs",
    "-Wl,--gc-sections"
)
$libs = @(
    "-L$devkitpro/portlibs/switch2/lib",
    "-L$devkitpro/libounce/lib",
    "-lkaihen_plugin",
    "-lounce_core"
)

Write-Host "Linking build/stream-module.elf..."
& $gcc $ldflags $objs $libs -o "build/stream-module.elf"
if ($LASTEXITCODE -ne 0) {
    Write-Error "Linking failed."
    exit 1
}

Write-Host "Creating stream-module.nso..."
& $elf2nso "build/stream-module.elf" "stream-module.nso"
if ($LASTEXITCODE -ne 0) {
    Write-Error "elf2nso failed."
    exit 1
}

Write-Host "Build successful: stream-module.nso" -ForegroundColor Green
