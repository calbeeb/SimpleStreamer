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
$elf2nro = "$devkitpro/tools/bin/elf2nro.exe"
$nacptool = "$devkitpro/tools/bin/nacptool.exe"

$appVersion = "0.4.0"
$appTitle   = "SimpleStreamer"
$appAuthor  = "calbeeb"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptDir

if ($Clean) {
    Write-Host "Cleaning..."
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue build, stream-conf.nro, build/stream-conf.elf
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

$sources = @("main.c", "ipc.c")
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
    "-lkaihen",
    "-lounce"
)

Write-Host "Linking build/stream-conf.elf..."
& $gcc $ldflags $objs $libs -o "build/stream-conf.elf"
if ($LASTEXITCODE -ne 0) {
    Write-Error "Linking failed."
    exit 1
}

Write-Host "Creating build/stream-conf.nacp..."
& $nacptool --create $appTitle $appAuthor $appVersion "build/stream-conf.nacp"
if ($LASTEXITCODE -ne 0) {
    Write-Error "nacptool failed."
    exit 1
}

Write-Host "Creating stream-conf.nro..."
& $elf2nro "build/stream-conf.elf" "stream-conf.nro" "--nacp=build/stream-conf.nacp"
if ($LASTEXITCODE -ne 0) {
    Write-Error "elf2nro failed."
    exit 1
}

Write-Host "Build successful: stream-conf.nro" -ForegroundColor Green
