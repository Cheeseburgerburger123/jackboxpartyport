# CI only: fills buildtools\ (next to ruffle-wiiu\) with the tools env.ps1 and build-wiiu.ps1 expect,
# under the exact folder names they use, then builds sa/tools/as-crfix. Run on a GitHub Windows runner.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bt = Join-Path $root 'buildtools'
$dl = Join-Path $bt '_downloads'
New-Item -ItemType Directory -Force $bt, $dl | Out-Null

function Fetch($urls, $file) {
    $out = Join-Path $dl $file
    if (Test-Path $out) { return $out }
    foreach ($url in @($urls)) {
        try {
            Write-Host "download $url"
            Invoke-WebRequest -Uri $url -OutFile $out -UseBasicParsing
            return $out
        } catch { Write-Host "  failed: $($_.Exception.Message)" }
    }
    throw "could not download $file"
}

function Unpack($archive, $dest) {
    New-Item -ItemType Directory -Force $dest | Out-Null
    if ($archive -like '*.zip') { Expand-Archive -Path $archive -DestinationPath $dest -Force }
    else { tar -xf $archive -C $dest; if ($LASTEXITCODE -ne 0) { throw "tar failed: $archive" } }
}

function Need($path, [scriptblock]$install) {
    if (Test-Path $path) { Write-Host "ok: $path"; return }
    & $install
    if (-not (Test-Path $path)) { throw "missing after install: $path" }
}

# Rust, with the Windows gnullvm host (links through llvm-mingw) and the wasm32-wasip1 target.
$env:RUSTUP_HOME = "$bt\rustup"
$env:CARGO_HOME = "$bt\cargo"
Need "$bt\cargo\bin\cargo.exe" {
    $init = Fetch 'https://win.rustup.rs/x86_64' 'rustup-init.exe'
    & $init -y --no-modify-path --profile minimal --default-host x86_64-pc-windows-gnullvm
    if ($LASTEXITCODE -ne 0) { throw 'rustup-init failed' }
}
& "$bt\cargo\bin\rustup.exe" target add wasm32-wasip1
if ($LASTEXITCODE -ne 0) { throw 'rustup target add failed' }

Need "$bt\jre\jdk-21.0.12.1+1-jre\bin\java.exe" {
    Unpack (Fetch 'https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.12.1%2B1/OpenJDK21U-jre_x64_windows_hotspot_21.0.12.1_1.zip' 'jre.zip') "$bt\jre"
}

Need "$bt\llvm-mingw-20261006-ucrt-x86_64\bin\clang.exe" {
    Unpack (Fetch 'https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/llvm-mingw-20261006-ucrt-x86_64.zip' 'llvm-mingw.zip') $bt
}

Need "$bt\clang+llvm-23.1.3-x86_64-pc-windows-msvc\bin\clang.exe" {
    Unpack (Fetch 'https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.3/clang+llvm-23.1.3-x86_64-pc-windows-msvc.tar.xz' 'llvm.tar.xz') $bt
}

Need "$bt\wabt\wabt-1.0.42\bin\wasm2c.exe" {
    Unpack (Fetch 'https://github.com/WebAssembly/wabt/releases/download/1.0.42/wabt-1.0.42-windows.tar.gz' 'wabt.tar.gz') "$bt\wabt"
}

Need "$bt\cmake-4.4.4-windows-x86_64\bin\cmake.exe" {
    Unpack (Fetch 'https://github.com/Kitware/CMake/releases/download/v4.4.4/cmake-4.4.4-windows-x86_64.zip' 'cmake.zip') $bt
}

Need "$bt\ninja\ninja.exe" {
    Unpack (Fetch 'https://github.com/ninja-build/ninja/releases/latest/download/ninja-win.zip' 'ninja.zip') "$bt\ninja"
}

# CafeGLSL for the console (static library linked into the app). The shaders themselves are already
# compiled in the repo (shaders/*.gsh); the workflow touches them so the Windows compiler isn't needed.
Need "$bt\cafeglsl-wiiu\cafeglsl-1.0.0-rc1-wiiu-ppc\lib\libcafeglsl.a" {
    $a = Fetch @(
        'https://github.com/Exzap/CafeGLSL/releases/download/v1.0.0-rc1/cafeglsl-1.0.0-rc1-wiiu-ppc.tar.xz',
        'https://github.com/Exzap/CafeGLSL/releases/download/1.0.0-rc1/cafeglsl-1.0.0-rc1-wiiu-ppc.tar.xz'
    ) 'cafeglsl-wiiu.tar.xz'
    Unpack $a "$bt\cafeglsl-wiiu"
}

# MSYS2 (preinstalled on the runner) + devkitPro (devkitPPC, wut, wiiu portlibs incl. SDL2).
if (-not (Test-Path "$bt\msys64")) {
    New-Item -ItemType Junction -Path "$bt\msys64" -Target 'C:\msys64' | Out-Null
}
$bash = "$bt\msys64\usr\bin\bash.exe"
$env:MSYSTEM = 'MSYS'
$env:CHERE_INVOKING = '1'
$dkp = @'
set -e
if ! grep -q '^\[dkp-libs\]' /etc/pacman.conf; then
  pacman-key --recv BC26F752D25B92CE272E0F44F7FD5492264BB9D0 --keyserver keyserver.ubuntu.com
  pacman-key --lsign BC26F752D25B92CE272E0F44F7FD5492264BB9D0
  curl -fsSL -o /tmp/devkitpro-keyring.pkg.tar.zst https://pkg.devkitpro.org/devkitpro-keyring.pkg.tar.zst
  pacman -U --noconfirm /tmp/devkitpro-keyring.pkg.tar.zst
  printf '\n[dkp-libs]\nServer = https://pkg.devkitpro.org/packages\n\n[dkp-windows]\nServer = https://pkg.devkitpro.org/packages/windows/$arch/\n' >> /etc/pacman.conf
fi
pacman -Sy --noconfirm --needed devkit-env wiiu-dev wiiu-sdl2 wiiu-pkg-config
'@
& $bash -lc $dkp
if ($LASTEXITCODE -ne 0) { throw 'devkitPro install failed' }
if (-not (Test-Path "$bt\msys64\opt\devkitpro\portlibs\wiiu\bin\powerpc-eabi-cmake")) { throw 'powerpc-eabi-cmake missing' }

# as-crfix: GCC finds it as as.exe through -B (host/wiiu/CMakeLists.txt).
$crfix = Join-Path $root 'sa\tools\as-crfix'
New-Item -ItemType Directory -Force "$crfix\bin" | Out-Null
& "$bt\llvm-mingw-20261006-ucrt-x86_64\bin\clang++.exe" -O2 -std=c++20 -static "$crfix\as_crfix.cpp" -o "$crfix\bin\as.exe"
if ($LASTEXITCODE -ne 0) { throw 'as-crfix build failed' }
'buildtools ready'
