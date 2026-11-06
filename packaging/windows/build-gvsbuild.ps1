# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build gtk4-frdp and FreeRDP 3 with Visual Studio (clang-cl) against a
# gvsbuild GTK 4 release, for applications that ship gvsbuild's GTK with
# python.org Python (MSYS2 builds cannot be mixed with it). Produces
# gtk4-frdp-<version>-gvsbuild-<gvsbuild version>-x64.zip with:
#   bin\    gtk4-frdp, FreeRDP, WinPR and OpenSSL DLLs
#   lib\girepository-1.0\GtkFrdp-4.0.typelib
#
# Run from a Visual Studio x64 developer shell with CMake, Ninja, clang-cl,
# Python 3.14 with setuptools (gvsbuild's g-ir-scanner is built for it and
# imports distutils) and vcpkg
# (VCPKG_INSTALLATION_ROOT). gtk4-frdp needs clang-cl, not cl: it uses
# GLib's g_autofree/g_autoptr cleanup macros.
param(
    [string]$GvsbuildVersion = "2026.8.0",
    [string]$GvsbuildSha256 = "1f95a92d037f5292da05e6ab1037032ff21ddb7b20d4ac8e83e3674c864c07b0",
    [string]$FreeRdpVersion = "3.32.1",
    [string]$FreeRdpSha256 = "3021cf8848efbd0064664e187cb61e6101e4e74a4d8ffb495d9f7715e22d67de",
    [string]$Work = "$PWD\_win"
)
$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true
$Source = (Resolve-Path "$PSScriptRoot\..\..").Path

function Get-Checked($Url, $Path, $Sha256) {
    if (-not (Test-Path $Path)) { Invoke-WebRequest -Uri $Url -OutFile $Path }
    $actual = (Get-FileHash -Algorithm SHA256 $Path).Hash.ToLower()
    if ($actual -ne $Sha256) { throw "${Path}: SHA-256 $actual, expected $Sha256" }
}

New-Item -ItemType Directory -Force $Work | Out-Null

# 1. gvsbuild GTK, extracted where its pkg-config files say it lives.
$Gtk = "C:\gtk-build\gtk\x64\release"
$zip = "$Work\GTK4_Gvsbuild_${GvsbuildVersion}_x64.zip"
Get-Checked "https://github.com/wingtk/gvsbuild/releases/download/$GvsbuildVersion/GTK4_Gvsbuild_${GvsbuildVersion}_x64.zip" $zip $GvsbuildSha256
if (-not (Test-Path "$Gtk\bin\gtk-4-1.dll")) { Expand-Archive -Force $zip $Gtk }

# 2. OpenSSL (gvsbuild has none) from vcpkg.
$Vcpkg = $env:VCPKG_INSTALLATION_ROOT
& "$Vcpkg\vcpkg.exe" install openssl:x64-windows
$Ssl = "$Vcpkg\installed\x64-windows"

# WinPR's built-in MD4/RC4 avoid a runtime dependency on OpenSSL's legacy
# provider, which is not included in this portable package.
# 3. FreeRDP: libraries and channels only. zlib comes from gvsbuild (its
#    zlib1.dll ships with GTK), so the zip does not carry a second copy.
$FreeRdp = "$Work\freerdp"
$tarball = "$Work\freerdp-$FreeRdpVersion.tar.gz"
Get-Checked "https://pub.freerdp.com/releases/freerdp-$FreeRdpVersion.tar.gz" $tarball $FreeRdpSha256
if (-not (Test-Path "$Work\freerdp-$FreeRdpVersion")) { tar -xzf $tarball -C $Work }
# The tarball sits inside this git checkout, and FreeRDP takes its version
# from the nearest git tag unless told not to: on a gtk4-frdp tag it builds
# itself as "0.1.0" (winpr0.pc, freerdp0.pc) and meson cannot find freerdp3.
cmake -S "$Work\freerdp-$FreeRdpVersion" -B "$Work\freerdp-build" -G Ninja `
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$FreeRdp" `
    -DCMAKE_PREFIX_PATH="$Ssl;$Gtk" -DOPENSSL_ROOT_DIR="$Ssl" -DZLIB_ROOT="$Gtk" `
    -DWITH_CLIENT=ON -DWITH_CLIENT_COMMON=ON -DWITH_CLIENT_SDL=OFF -DWITH_CLIENT_WINDOWS=OFF `
    -DWITH_SERVER=OFF -DWITH_SAMPLE=OFF -DWITH_MANPAGES=OFF -DWITH_PLATFORM_SERVER=OFF `
    -DWITH_FFMPEG=OFF -DWITH_SWSCALE=OFF -DWITH_DSP_FFMPEG=OFF -DWITH_OPENH264=OFF `
    -DWITH_KRB5=OFF -DWITH_AAD=OFF -DWITH_WEBVIEW=OFF -DWITH_JSON_DISABLED=ON `
    -DWITH_URIPARSER=OFF -DWITH_CAIRO=OFF -DCHANNEL_URBDRC=OFF -DWITH_WINPR_TOOLS=OFF `
    -DBUILD_TESTING=OFF -DWITH_MEDIA_FOUNDATION=OFF `
    -DWITH_INTERNAL_MD4=ON -DWITH_INTERNAL_RC4=ON `
    -DUSE_VERSION_FROM_GIT_TAG=OFF
cmake --build "$Work\freerdp-build"
cmake --install "$Work\freerdp-build"

# 4. gtk4-frdp with clang-cl. gvsbuild's g-ir-scanner script names the
#    gvsbuild CI machine's Python; run it with this one instead.
$py = (Get-Command python).Source.Replace('\', '/')
$gtkFwd = $Gtk.Replace('\', '/')
@"
[binaries]
c = 'clang-cl'
g-ir-scanner = ['$py', '$gtkFwd/bin/g-ir-scanner']
g-ir-compiler = '$gtkFwd/bin/g-ir-compiler.exe'
pkg-config = '$gtkFwd/bin/pkgconf.exe'
"@ | Set-Content -Encoding ascii "$Work\native.ini"
$env:PKG_CONFIG_PATH = "$Gtk\lib\pkgconfig;$FreeRdp\lib\pkgconfig"
$env:PATH = "$Gtk\bin;$FreeRdp\bin;$Ssl\bin;$env:PATH"
$env:LIB = "$Gtk\lib;$env:LIB"
$env:INCLUDE = "$Gtk\include;$env:INCLUDE"
$Prefix = "$Work\prefix"
meson setup "$Work\build" $Source --native-file "$Work\native.ini" --buildtype=release `
    --prefix="$Prefix" -Dtests=false -Dexamples=false -Ddocumentation=false
meson compile -C "$Work\build"
meson install -C "$Work\build"

# 5. Smoke test: the widget loads in gvsbuild's PyGObject.
$wheels = Get-ChildItem "$Gtk\wheels" -Filter *.whl | Where-Object { $_.Name -match "^(pygobject|pycairo)-.*-cp314-" }
python -m pip install --quiet $wheels.FullName
$env:PATH = "$Prefix\bin;$env:PATH"
$env:GI_TYPELIB_PATH = "$Prefix\lib\girepository-1.0;$Gtk\lib\girepository-1.0"
python -c "import gi; gi.require_version('GtkFrdp', '4.0'); from gi.repository import GtkFrdp; d = GtkFrdp.Display(); print('GtkFrdp', GtkFrdp.MAJOR_VERSION, GtkFrdp.MINOR_VERSION, GtkFrdp.MICRO_VERSION, 'loads:', type(d).__name__)"

# 6. Package.
$version = (Select-String -Path "$Source\meson.build" -Pattern "version\s*:\s*'([^']+)'").Matches[0].Groups[1].Value
$stage = "$Work\stage"
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$stage\bin", "$stage\lib\girepository-1.0" | Out-Null
Copy-Item "$Prefix\bin\*.dll" "$stage\bin"
Copy-Item "$FreeRdp\bin\freerdp3.dll", "$FreeRdp\bin\freerdp-client3.dll", "$FreeRdp\bin\winpr3.dll" "$stage\bin"
Copy-Item "$Ssl\bin\libssl-3-x64.dll", "$Ssl\bin\libcrypto-3-x64.dll" "$stage\bin"
Copy-Item "$Prefix\lib\girepository-1.0\GtkFrdp-4.0.typelib" "$stage\lib\girepository-1.0"
Copy-Item "$Source\COPYING", "$Source\NOTICE" $stage
# Check the staged DLLs, not build-tree copies, before publishing the zip.
python "$Source\packaging\windows\check-crypto.py" "$stage\bin" "$Gtk\bin"
if ($LASTEXITCODE -ne 0) { throw "Staged WinPR MD4/RC4 verification failed" }
$out = "$PWD\gtk4-frdp-$version-gvsbuild-$GvsbuildVersion-x64.zip"
Compress-Archive -Force "$stage\*" $out
Get-FileHash -Algorithm SHA256 $out | ForEach-Object { "$($_.Hash.ToLower())  $(Split-Path -Leaf $out)" } | Tee-Object "$out.sha256"
