# Build Instructions

This document provides instructions for building Cool Live Captions from source on Windows, Linux, and macOS.

## Requirements

- CMake 3.31.X - Please note that 4.0 versions or newer may work but are not guaranteed to be compatible.
- C++20 toolchain (GCC/Clang/MinGW)
- ONNX Runtime (auto-fetched by cmake if not provided)
- april-asr (auto-fetched by cmake if not provided)

Tested CMake version:
- 3.31.6 from Debian 13 Trixie for Linux
- 3.31.10 from MS Visual Studio Build Tools 2026 (18.2.1) for Windows

## Getting Started

1. Clone the repo.
2. Configure and build with your preferred GCC/Clang toolchain (examples below). Run the produced `bin/coollivecaptions` executable.
3. To provide models for testing, you have several options:
    - Manual model: place model files directly into the per-user models folder (see above).
      1. Place the model file into your models folder (created on first run or by installer):
         - Windows: `%LOCALAPPDATA%/coollivecaptions/models`
         - macOS: `~/Library/Application Support/com.batterydie.coollivecaptions/models`
         - Linux: `~/.coollivecaptions/models`
    - Local manifest:
      1. Create a manifest.json that follows the format in the "Model Management" section.
      2. Serve it from a local HTTP server, e.g. run `python -m http.server 8000` in the folder with manifest.json.
      3. Launch the app with the `--dev-manifest` argument; the app will fetch `http://localhost:8000/manifest.json` for testing.
    - Remote manifest: host your manifest on any reachable URL and use the same `--dev-manifest` flow when testing.
    - Quick local-only testing: manually copy model files into the models folder (no manifest or server required).

> NOTICE: Our **own** models are under development and will be available soon. You can also use other models provided by abb128's april-asr: [https://abb128.github.io/april-asr/models.html](https://abb128.github.io/april-asr/models.html)

### Build: Windows (MSVC)

```powershell
cmake -S . -B build-windows-msvc-release -G "Visual Studio 18 2026" -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build-windows-msvc-release --config Release
```
```powershell
cmake -S . -B build-windows-msvc-debug -G "Visual Studio 18 2026" -A x64 -DCMAKE_BUILD_TYPE=Debug
cmake --build build-windows-msvc-debug --config Debug
```

The build copies ONNX/april-asr DLLs next to the exe for redistribution.

### Build: Windows (MinGW)

```powershell
cmake -S . -B build-windows-release -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-windows-release
```
```powershell
cmake -S . -B build-windows-debug -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build-windows-debug
```

The build copies ONNX/april-asr DLLs and MinGW runtimes (`libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll`, `libsrdc.dll`) next to the exe for redistribution.

The anti-virus false positives are common for MinGW builds. If your build is flagged, two options:
- Add an exception for the build folder in your anti-virus software.
- Build with MSVC instead, which is less likely to be flagged.

### Build: Linux (GCC/Clang)

Install the required packages (tested on Debian/Ubuntu-based distros):
```bash
sudo apt install \
  dpkg-dev \
  ninja-build \
  libgl1-mesa-dev \
  libglu1-mesa-dev \
  libx11-dev \
  libxrandr-dev \
  libxinerama-dev \
  libxi-dev \
  libxcursor-dev \
  libxkbcommon-dev \
  libwayland-dev \
  wayland-protocols \
  libpipewire-0.3-dev \
  libspa-0.2-dev
```

```bash
cmake -S . -B build-linux-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux-release
```
```bash
cmake -S . -B build-linux-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-linux-debug
```

```bash
cmake -S . -B build-linux-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux-release
```
```bash
cmake -S . -B build-linux-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-linux-debug
```

### Build: macOS (Clang)

```bash
cmake -S . -B build-macos-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-macos-release
```
```bash
cmake -S . -B build-macos-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-macos-debug
```

### Run

CMake creates a `bin` folder in the build directory (for example, `Cool-Live-Captions\build\bin\coollivecaptions.exe`). Run the executable to start the app.

You can also run it from the command line to view logs and pass arguments such as `--dev-manifest` for testing.

## Model Management

Cool Live Captions supports downloading and updating models from a remote manifest file in JSON format. The manifest file should be an array of model objects with the following fields:

```json
[
  {
    "id": "Example English Model",
    "name": "Example English Model",
    "version": "1.0",
    "author": "Example Author One",
    "language": "en",
    "description": "Trained on clean speech with numbers and punctuation, good for general use",
    "url": "http://localhost:8000/models/example_english.april",
    "url_website": "https://localhost:8000/models.html",
    "filename": "example_english.april",
    "size_bytes": 328789000
  },
  {
    "id": "Example French Model",
    "name": "Example French Model Dev",
    "version": "0.1",
    "author": "Example Author Two",
    "language": "fr",
    "description": "Trained on clean speech with numbers and punctuation, in development",
    "url": "http://localhost:8000/models/example_french_dev.april",
    "url_website": "https://localhost:8000/models.html",
    "filename": "example_french_dev.april",
    "size_bytes": 328789000
  }
]
```

To test your local manifest, start the app with the `--dev-manifest` argument. Host your manifest JSON at `http://localhost:8000/manifest.json` using any web server of your choice.