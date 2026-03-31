## Cool Live Captions

A free and open source live caption desktop application that converts audio from your microphone or system audio to text in real-time. The speech recognition is powered by april-asr library with ONNX format. All processed on-device using your CPU.

> Disclaimer: The captions may not be 100% accurate. Please do not rely on it for critical purposes.

### OS Support

- Windows 10 or later via WASAPI (Tested on Windows 11, x86-64 only)
- Linux 6.x kernel or later via PipeWire (Tested on Debian 13/14 and Ubuntu 22.04/24.04 LTS, x86-64 only)
- macOS Sonoma 14.4 or later via AudioCore (Tested on macOS Tahoe, Apple Silicon only)

## Getting Started

### Users

1. Download the latest release for your platform.
2. Install and launch the app.
3. The app will ask you to download a model if no models are found in your models folder, click "Yes".
4. Download and install a model from the list.
5. Once a model is loaded, the live captions will start immediately.

> NOTICE: Our **own** models are under development and will be available soon. You can also use other models provided by abb128's april-asr: [https://abb128.github.io/april-asr/models.html](https://abb128.github.io/april-asr/models.html).

### Developers

Please see [BUILD.md](BUILD.md) for build instructions.

## Development Status

| Feature | Description | Status |
| --- | --- | --- |
| Dear ImGui | GUI framework for desktop UI | Implemented |
| On-device STT | april-asr + ONNX Runtime support | Implemented |
| Audio Capture | Loopback, Microphone, Specific app | Implemented |
| Autosave Transcript | Save captions to text file | Implemented |
| Profanity Filter | Filter out profane words | Implemented |
| Model Managment | Download and manage models | Implemented |
| Hosting Model | Provide our own models | Coming Soon |
| Auto-Correction | Correct common recognition errors | Coming Soon |
| Customization | Text size, colour, background | Implemented |
| Multi-Platform | Windows, Linux, macOS | Implemented |
| Localization | Support multiple languages | Partially Implemented |

## Acknowledgements

This project makes use of a few libraries:

- [april-asr](https://github.com/abb128/april-asr) - for on-device speech-to-text/speech recognition (License: GPL-3.0, © abb128 and contributors)
- [ONNX Runtime](https://onnxruntime.ai/) - for running ONNX models efficiently (License: MIT, © Microsoft Corporation)
- [Dear ImGui](https://github.com/ocornut/imgui) - for the GUI framework (License: MIT, © Omar Cornut and contributors)
- [GLFW](https://www.glfw.org/) - for creating windows, contexts, and managing input (License: zlib/libpng, © Camilla Löwy)

I would like to thank to abb128 (and contributors of april-asr) for creating april-asr libary.

For more detials, please see the [THIRD-PARTY-NOTICES](THIRD-PARTY-NOTICES) file.

## License

Cool Live Captions is free software licensed under GPL-3.0. See [LICENSE](LICENSE) for details.