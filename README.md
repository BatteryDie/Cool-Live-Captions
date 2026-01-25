## Cool Live Captions
A free and open source live caption desktop application that converts audio from your microphone or system audio to text in real-time. The speech recognition is powered by april-asr library with ONNX format. All processed on-device using your CPU.

> Disclaimer: Cool Live Captions is experimental and captions may not be 100% accurate.

### OS Audio API

Here's what we are using for audio capture on each OS offered:

| OS      | API            | Loopback | Microphone | Capture Specific App |
|---------|----------------|:--------:|:----------:|:--------------------:|
| Windows | WASAPI         | ✅       | ✅         | ✅                   |
| Linux   | PipeWire       | ✅       | ✅         | ✅                   |
| macOS   | Core Audio     | ❌       | ✅         | ✅                   |

> For Linux, we are currently using PipeWire over PulseAudio on Linux due to better support for loopback capture and specific app capture.

> macOS 14.2 introduced a Core Audio API for creating a “Process Tap” that can capture audio from a specific app, a subset of apps, or an output device, and attach it to a private or public Aggregate Device. See [AudioHardwareCreateProcessTap](https://developer.apple.com/documentation/coreaudio/4160724-audiohardwarecreateprocesstap?language=objc). Third-party virtual devices like [BlackHole](https://github.com/ExistentialAudio/BlackHole) remain an alternative to Loopback.

### Location for Models, Transcripts, and Settings
- Models are loaded from your per-user models folder:
  - Windows: `%LOCALAPPDATA%/coollivecaptions/models`
  - macOS: `~/Library/Application Support/com.batterydie.coollivecaptions/models`
  - Linux: `~/.coollivecaptions/models`
- Transcripts are saved to `{Documents}/Cool Live Captions/transcription-{timestamp}.txt` by default.
- Settings are stored in the user config directory:
  - Windows: `%LOCALAPPDATA%/coollivecaptions/settings.ini`
  - macOS: `~/Library/Application Support/com.batterydie.coollivecaptions/settings.ini`
  - Linux: `~/.config/coollivecaptions/settings.ini`

### Requirements
- CMake 3.24
- C++20 toolchain (GCC/Clang/MinGW)
- ONNX Runtime (auto-fetched by cmake if not provided)
- april-asr (auto-fetched by cmake if not provided)

## Getting Started

### Quick Start for Users

1. Download the latest release for your platform.
2. Install and launch the app.
3. The app will ask you to download a model if no models are found in your models folder, click "Yes".
4. Download and install a model from the list.
5. Once a model is loaded, the live captions will start immediately.

### Quick Start for Developers
1. Clone the repo.
2. Configure and build with your preferred GCC/Clang toolchain (examples below). Run the produced `bin/coollivecaptions` executable.
3. To provide models for testing, you have several options:
    - Manual model: place model files directly into the per-user models folder (see above).
      1. Download `april-english-dev-01110_en.april` model from: [https://abb128.github.io/april-asr/models.html](https://abb128.github.io/april-asr/models.html)
      2. Place the model file into your models folder (created on first run or by installer):
         - Windows: `%LOCALAPPDATA%/coollivecaptions/models`
         - macOS: `~/Library/Application Support/com.batterydie.coollivecaptions/models`
         - Linux: `~/.coollivecaptions/models`
    - Local manifest:
      1. Create a manifest.json that follows the format in the "Model Management" section.
      2. Serve it from a local HTTP server, e.g. run `python -m http.server 8000` in the folder with manifest.json.
      3. Launch the app with the `--dev-manifest` argument; the app will fetch `http://localhost:8000/manifest.json` for testing.
    - Remote manifest: host your manifest on any reachable URL and use the same `--dev-manifest` flow when testing.
    - Quick local-only testing: manually copy model files into the models folder (no manifest or server required).

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

### Build: Linux (GCC/Clang)

```bash
cmake -S . -B build-linux-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux-release
```
```bash
cmake -S . -B build-linux-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-linux-debug
```

To build and test AppImage, please refer to `deploy/build_appimage.sh` script.

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
Place models in your per-user models folder (see above). Launch the app; choose Audio Source and Caption Model from the menubar. Captions appear in the main window and are saved to `{Documents}/Cool Live Captions/transcript-{timestamp}.md`.

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

## Acknowledgements

This project makes use of a few libraries:

- [april-asr](https://github.com/abb128/april-asr) - for on-device speech-to-text/speech recognition (License: GPL-3.0, © abb128 and contributors)
- [ONNX Runtime](https://onnxruntime.ai/) - for running ONNX models efficiently (License: MIT, © Microsoft Corporation)
- [Dear ImGui](https://github.com/ocornut/imgui) - for the GUI framework (License: MIT, © Omar Cornut and contributors)

I would like to thank to abb128 (and contributors of april-asr) for creating april-asr libary.

## License

Cool Live Captions is free software licensed under GPL-3.0. See [LICENSE](LICENSE) for details.