# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

HandBrake is an open-source video transcoder for Linux, macOS, and Windows that converts video files into widely compatible formats (MP4, MKV, WebM). It supports hardware acceleration and processes various sources including DVDs and Blu-rays.

## Common Development Commands

### Building on Linux
```bash
# Basic build
./configure --launch-jobs=$(nproc) --launch

# Build with hardware acceleration support
./configure --enable-qsv --enable-vce --enable-nvenc --enable-nvdec --launch-jobs=$(nproc) --launch

# Build CLI only (no GUI)
./configure --disable-gtk --launch-jobs=$(nproc) --launch

# Install after building
sudo make --directory=build install

# Clean build
rm -rf build
```

### Building on macOS
```bash
# Configure and build
./configure --launch-jobs=$(sysctl -n hw.activecpu) --launch

# Xcode project is located in macosx/
# Use xcodemake script for Xcode builds
make/xcodemake
```

### Building on Windows
```bash
# Windows builds use MinGW-w64 cross-compilation
# GUI is in win/CS/ (C#/.NET solution)
```

### View all configuration options
```bash
./configure --help
```

## High-Level Architecture

### Core Components

1. **libhb/** - Core transcoding library
   - Contains the video/audio processing engine
   - Handles encoding, decoding, filtering, and muxing
   - Key files: hb.c (main library), work.c (job processing), common.c (utilities)
   - Hardware acceleration: qsv_common.c (Intel), nvenc_common.c (NVIDIA), vce_common.c (AMD)

2. **Platform-Specific Frontends**
   - **gtk/** - Linux GUI using GTK 4 and Meson build system
   - **macosx/** - macOS native GUI (Objective-C, Xcode project)
   - **win/CS/** - Windows GUI (C#/.NET WPF, Visual Studio solution)

3. **contrib/** - Third-party dependencies
   - Managed through module.defs/module.rules files
   - Includes: FFmpeg, x264, x265, SVT-AV1, libdav1d, FDK-AAC, etc.
   - Each dependency has its own build configuration

4. **Build System**
   - Python-based configure script (make/configure.py)
   - Creates platform-specific build files
   - Handles dependency downloading and compilation

### Key Architectural Patterns

- **Pipeline Architecture**: Video processing uses a pipeline of filters and encoders
- **Job Queue System**: Transcoding jobs are queued and processed sequentially
- **Hardware Abstraction**: Unified interface for different hardware acceleration APIs
- **Preset System**: Predefined encoding settings for various devices and use cases

## Project Structure

```
HandBrake/
├── libhb/              # Core transcoding library
│   ├── handbrake/      # Public headers
│   └── platform/       # Platform-specific code
├── gtk/                # Linux GUI
│   ├── src/            # GTK application source
│   └── po/             # Translations
├── macosx/             # macOS GUI
│   ├── HandBrakeKit/   # macOS framework
│   └── *.xcodeproj     # Xcode project
├── win/CS/             # Windows GUI
│   ├── HandBrakeWPF/   # WPF application
│   └── HandBrake.sln   # Visual Studio solution
├── contrib/            # Dependencies
├── make/               # Build system
├── scripts/            # Utility scripts
└── test/               # CLI test tool
```

## Key Integration Points

- **Video Processing**: libhb/work.c coordinates the transcoding pipeline
- **Hardware Acceleration**: Check libhb/*_common.c for vendor-specific implementations  
- **GUI Communication**: Frontends communicate with libhb via hb_json.c API
- **Presets**: libhb/preset.c manages encoding presets
- **Container Formats**: libhb/muxavformat.c handles output muxing

## Testing and Development

- **CLI Testing**: Use test/test.c (HandBrakeCLI) for command-line testing
- **Debugging**: Build with debug symbols using configure options
- **Performance**: Use hardware acceleration options for testing performance

## Important Notes

- Always check existing code patterns in neighboring files before implementing new features
- Hardware acceleration code is platform and vendor specific
- GUI changes require platform-specific expertise (GTK, Cocoa, or WPF)
- Major feature additions require discussion with the HandBrake team
- Follow the GitHub pull request workflow for contributions