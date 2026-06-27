# Build Environment Notes

This document records the local Windows build setup performed for this repository, including installed tools, downloaded files, build options, and cleanup steps.

## Build Summary

The repository was built on Windows with MSVC, CMake, Ninja, and vcpkg.

Generated executable:

```text
D:\github\aviateur\cmake-build-release\bin\aviateur.exe
```

The successful build used these options:

```powershell
cmake -S . -B cmake-build-release `
  -G Ninja `
  -DCMAKE_MAKE_PROGRAM=D:\vcpkg\downloads\tools\ninja-1.13.2-windows\ninja.exe `
  -DCMAKE_BUILD_TYPE=Release `
  -DAVIATEUR_ENABLE_GSTREAMER=OFF `
  -DREVECTOR_VULKAN=OFF `
  -DPATHFINDER_BACKEND_VULKAN=OFF `
  -DCMAKE_TOOLCHAIN_FILE=D:\vcpkg\scripts\buildsystems\vcpkg.cmake
```

GStreamer was disabled for this build because the project currently hardcodes the Windows GStreamer path to `C:\Program Files\gstreamer\1.0\msvc_x86_64`, while GStreamer was installed under the current user's profile without administrator permissions.

The Revector/Pathfinder Vulkan backend was disabled because Vulkan SDK headers were not installed. The earlier failure was a missing `vulkan/vk_platform.h` include.

## Installed Tools And Packages

### Visual Studio Build Tools 2022

Installed location:

```text
C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
```

Purpose:

- Provides MSVC `cl.exe`, linker, Windows SDK, and Visual Studio CMake.

Relevant files used during build:

```text
C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
```

### vcpkg

Installed location:

```text
D:\vcpkg
```

Installed using:

```powershell
git clone https://github.com/microsoft/vcpkg.git D:\vcpkg
D:\vcpkg\bootstrap-vcpkg.bat
D:\vcpkg\vcpkg.exe integrate install
```

User environment variable added:

```text
VCPKG_ROOT=D:\vcpkg
```

vcpkg packages installed for the successful build:

```powershell
D:\vcpkg\vcpkg.exe install libusb ffmpeg libsodium fmt pkgconf --triplet x64-windows
```

Installed package root:

```text
D:\vcpkg\installed\x64-windows
```

Important tools downloaded by vcpkg during setup:

```text
D:\vcpkg\downloads\tools\ninja-1.13.2-windows\ninja.exe
```

### GStreamer

Installed location:

```text
C:\Users\31499\AppData\Local\Programs\gstreamer\1.0\msvc_x86_64
```

GStreamer was installed from the official MSVC x86_64 installer. It was not used in the successful build because `AVIATEUR_ENABLE_GSTREAMER=OFF` was passed to CMake.

An attempt was made to add the default hardcoded GStreamer bin path to the user `PATH`:

```text
C:\Program Files\gstreamer\1.0\msvc_x86_64\bin
```

Because the actual installation is under the user profile, check both paths when cleaning up.

## Downloaded Files

Temporary downloads were stored under:

```text
C:\Users\31499\AppData\Local\Temp\opencode
```

Files downloaded manually during setup:

```text
C:\Users\31499\AppData\Local\Temp\opencode\vs_BuildTools.exe
C:\Users\31499\AppData\Local\Temp\opencode\gstreamer-1.0-msvc-x86_64-1.28.4.exe
C:\Users\31499\AppData\Local\Temp\opencode\gstreamer-1.0-msvc-x86_64-1.28.4-redownload.exe
C:\Users\31499\AppData\Local\Temp\opencode\gstreamer-install-redownload.log
C:\Users\31499\AppData\Local\Temp\opencode\cmake-4.3.3-windows-x86_64.zip
```

The first GStreamer installer download was corrupt and was replaced by the `*-redownload.exe` copy.

Some files were manually copied into the vcpkg download cache to work around proxy or GitHub download interruptions:

```text
D:\vcpkg\downloads\cmake-4.3.3-windows-x86_64.zip
D:\vcpkg\downloads\PowerShell-7.6.2-win-x64.zip
D:\vcpkg\downloads\ninja-win-1.13.2.zip
D:\vcpkg\downloads\pkgconf-pkgconf-pkgconf-2.5.1.tar.gz
D:\vcpkg\downloads\ffmpeg-ffmpeg-n8.1.2.tar.gz
```

## Repository Changes From Build

Submodules were initialized with:

```powershell
git submodule update --init --recursive
```

This populated third-party source directories under:

```text
D:\github\aviateur\3rd
```

The build directory was created at:

```text
D:\github\aviateur\cmake-build-release
```

The build directory is generated output and should not be committed.

## opencode Configuration Change

The global opencode config was changed outside this repository:

```text
C:\Users\31499\.config\opencode\opencode.json
```

The current model context limit was changed from `128000` to `256000`:

```json
"context": 256000
```

Restart opencode for that change to take effect.

## Rebuild Commands

From this repository root:

```powershell
cmd /c "call ""C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"" && set VCPKG_ROOT=D:\vcpkg && ""C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"" -S . -B cmake-build-release -G Ninja -DCMAKE_MAKE_PROGRAM=D:\vcpkg\downloads\tools\ninja-1.13.2-windows\ninja.exe -DCMAKE_BUILD_TYPE=Release -DAVIATEUR_ENABLE_GSTREAMER=OFF -DREVECTOR_VULKAN=OFF -DPATHFINDER_BACKEND_VULKAN=OFF -DCMAKE_TOOLCHAIN_FILE=D:\vcpkg\scripts\buildsystems\vcpkg.cmake"
```

Then build:

```powershell
cmd /c "call ""C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"" && set VCPKG_ROOT=D:\vcpkg && ""C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"" --build cmake-build-release --config Release"
```

## Cleanup

### Remove Repository Build Output

```powershell
Remove-Item -Recurse -Force "D:\github\aviateur\cmake-build-release"
```

### Remove Temporary Downloads

```powershell
Remove-Item -Recurse -Force "C:\Users\31499\AppData\Local\Temp\opencode"
```

### Remove vcpkg Packages Only

Use this if keeping the vcpkg installation:

```powershell
D:\vcpkg\vcpkg.exe remove libusb ffmpeg libsodium fmt pkgconf --triplet x64-windows
```

### Remove vcpkg Completely

Use this if `D:\vcpkg` was only installed for this project:

```powershell
D:\vcpkg\vcpkg.exe integrate remove
Remove-Item -Recurse -Force "D:\vcpkg"
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", $null, "User")
```

### Remove GStreamer

Preferred method:

```text
Windows Settings -> Apps -> Installed apps -> GStreamer -> Uninstall
```

If it is not listed, remove the user install directory:

```powershell
Remove-Item -Recurse -Force "C:\Users\31499\AppData\Local\Programs\gstreamer"
```

Remove possible GStreamer entries from the user `PATH`:

```powershell
$remove = "C:\Program Files\gstreamer\1.0\msvc_x86_64\bin"
$path = [Environment]::GetEnvironmentVariable("Path", "User")
$newPath = (($path -split ";") | Where-Object { $_ -and $_ -ne $remove }) -join ";"
[Environment]::SetEnvironmentVariable("Path", $newPath, "User")

$remove = "C:\Users\31499\AppData\Local\Programs\gstreamer\1.0\msvc_x86_64\bin"
$path = [Environment]::GetEnvironmentVariable("Path", "User")
$newPath = (($path -split ";") | Where-Object { $_ -and $_ -ne $remove }) -join ";"
[Environment]::SetEnvironmentVariable("Path", $newPath, "User")
```

### Remove Visual Studio Build Tools

Use Visual Studio Installer:

```text
Start Menu -> Visual Studio Installer -> Build Tools 2022 -> More -> Uninstall
```

Do not manually delete the Visual Studio Build Tools directory.

### Revert opencode Context Limit

Edit:

```text
C:\Users\31499\.config\opencode\opencode.json
```

Change:

```json
"context": 256000
```

back to:

```json
"context": 128000
```

Restart opencode after editing the config.
