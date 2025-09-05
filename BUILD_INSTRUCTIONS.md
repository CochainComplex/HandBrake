# HandBrake Build Instructions with VAAPI Support

## Prerequisites
```bash
# Install required dependencies (Ubuntu/Debian)
sudo apt-get install autoconf automake build-essential cmake git libass-dev \
  libbz2-dev libfontconfig-dev libfreetype-dev libfribidi-dev \
  libharfbuzz-dev libjansson-dev liblzma-dev libmp3lame-dev \
  libnuma-dev libogg-dev libopus-dev libsamplerate0-dev libspeex-dev \
  libtheora-dev libtool libtool-bin libturbojpeg0-dev libvorbis-dev \
  libvpx-dev libx264-dev libx265-dev libxml2-dev libzimg-dev m4 make \
  meson nasm ninja-build patch pkg-config tar yasm zlib1g-dev \
  libva-dev libdrm-dev clang llvm

# For GUI support (optional)
sudo apt-get install libgtk-3-dev
```

## Build Commands

### Clean Build with VAAPI Support
```bash
# Navigate to HandBrake directory
cd /home/awarth/Devstuff/HandBrake

# Clean any previous builds
rm -rf build

# Configure AND build with VAAPI enabled (--launch does both)
python3 make/configure.py --enable-vaapi --launch-jobs=$(nproc) --launch

# If GTK4 is missing and you only need CLI (no GUI):
python3 make/configure.py --enable-vaapi --disable-gtk --launch-jobs=$(nproc) --launch

# Note: The --launch flag tells configure.py to immediately start building after configuration
# Without --launch, it would only configure and you'd need to run 'make' separately:
# python3 make/configure.py --enable-vaapi  # Configure only
# cd build && make -j$(nproc)              # Then build manually
```

### Quick Rebuild (if already configured)
```bash
cd /home/awarth/Devstuff/HandBrake/build
make -j$(nproc)
```

### Build with additional hardware acceleration (optional)
```bash
# With NVIDIA support
python3 make/configure.py --enable-vaapi --enable-nvenc --enable-nvdec --launch-jobs=$(nproc) --launch

# With Intel QSV support
python3 make/configure.py --enable-vaapi --enable-qsv --launch-jobs=$(nproc) --launch
```

## FFmpeg vs HandBrake VAAPI Commands Comparison

### Direct FFmpeg VAAPI Encoding
```bash
# FFmpeg H.264 VAAPI encoding
ffmpeg -vaapi_device /dev/dri/renderD128 -i input.mp4 \
  -vf 'format=nv12,hwupload' \
  -c:v h264_vaapi -b:v 5M -maxrate 5M \
  output_ffmpeg_vaapi.mp4

# FFmpeg H.265 VAAPI encoding
ffmpeg -vaapi_device /dev/dri/renderD128 -i input.mp4 \
  -vf 'format=nv12,hwupload' \
  -c:v hevc_vaapi -qp 25 \
  output_ffmpeg_hevc.mp4

# FFmpeg with quality-based encoding
ffmpeg -vaapi_device /dev/dri/renderD128 -i input.mp4 \
  -vf 'format=nv12,hwupload' \
  -c:v h264_vaapi -rc_mode CQP -qp 22 \
  output_ffmpeg_cqp.mp4
```

### HandBrake VAAPI Encoding (Equivalent)
```bash
# HandBrake H.264 VAAPI - equivalent to FFmpeg above
./build/HandBrakeCLI -i input.mp4 -o output_handbrake_vaapi.mp4 \
  --encoder vaapi_h264 \
  --vb 5000 \
  --two-pass --turbo

# HandBrake H.265 VAAPI 
./build/HandBrakeCLI -i input.mp4 -o output_handbrake_hevc.mp4 \
  --encoder vaapi_h265 \
  --quality 25

# HandBrake with constant quality (recommended)
./build/HandBrakeCLI -i input.mp4 -o output_handbrake_cq.mp4 \
  --encoder vaapi_h264 \
  --quality 22
```

### Key Differences:
- **FFmpeg**: Requires explicit VAAPI device and filter chain setup
- **HandBrake**: Automatically handles device selection and format conversion
- **FFmpeg**: Uses `-qp` or `-b:v` for quality/bitrate
- **HandBrake**: Uses `--quality` (RF) or `--vb` for quality/bitrate
- **HandBrake**: Includes better default settings and presets

## Testing VAAPI Support

### 1. Check if VAAPI encoders are available
```bash
# List all available encoders
./build/HandBrakeCLI --help | grep -i vaapi

# Or check encoder list
./build/HandBrakeCLI --encoder-list | grep -i vaapi
```

### 2. Test VAAPI H.264 encoding
```bash
# Create a test video file if it doesn't exist
ffmpeg -f lavfi -i testsrc=duration=10:size=1920x1080:rate=30 \
  -f lavfi -i sine=frequency=1000:duration=10 \
  -c:v libx264 -preset ultrafast -c:a aac \
  test_input.mp4

# Basic VAAPI H.264 test with HandBrake
./build/HandBrakeCLI -i test_input.mp4 -o test_vaapi_h264.mp4 \
  --encoder vaapi_h264 --quality 22 --verbose=1

# Equivalent FFmpeg command for the above HandBrake test:
ffmpeg -vaapi_device /dev/dri/renderD128 -i test_input.mp4 \
  -vf 'format=nv12,hwupload' \
  -c:v h264_vaapi -rc_mode CQP -qp 22 \
  -c:a copy \
  test_vaapi_h264_ffmpeg.mp4

# With more options
./build/HandBrakeCLI -i input.mp4 -o output_vaapi_h264.mp4 \
  --encoder vaapi_h264 \
  --quality 22 \
  --preset "Fast 1080p30" \
  --verbose=1
```

### 3. Test VAAPI H.265/HEVC encoding
```bash
# Basic VAAPI H.265 test
./build/HandBrakeCLI -i build/test.mp4 -o test_vaapi_h265.mp4 \
  --encoder vaapi_h265 --quality 24 --verbose=1

# 10-bit H.265 (if supported by hardware)
./build/HandBrakeCLI -i input.mp4 -o output_vaapi_h265_10bit.mp4 \
  --encoder vaapi_h265_10bit \
  --quality 24 \
  --verbose=1
```

### 4. Monitor VAAPI usage
```bash
# In another terminal, monitor GPU usage during encoding
watch -n 0.5 'sudo cat /sys/kernel/debug/dri/0/amdgpu_pm_info | grep -A 5 "GPU Load"'

# Or use radeontop for AMD GPUs
sudo radeontop
```

## Verify VAAPI System Support

### Check VAAPI availability on your system
```bash
# Check VA-API support
vainfo

# Check DRI devices
ls -la /dev/dri/

# Check if user has video group permissions
groups | grep video
```

## Troubleshooting

### If VAAPI encoders show as "invalid":
1. Ensure FFmpeg was built with VAAPI support:
   ```bash
   grep "enable-vaapi" build/contrib/ffmpeg/.stamp.ffmpeg.configure
   ```

2. Check HandBrake VAAPI detection logs:
   ```bash
   ./build/HandBrakeCLI --verbose=3 --encoder-list 2>&1 | grep -i vaapi
   ```

3. Verify system VAAPI support:
   ```bash
   vainfo
   ```

### Common Issues:
- **No VAAPI encoders**: FFmpeg wasn't built with VAAPI support (check contrib/ffmpeg/module.defs)
- **Permission denied**: Add user to video/render groups: `sudo usermod -a -G video,render $USER`
- **Driver issues**: Install appropriate VA drivers (mesa-va-drivers for AMD, intel-media-va-driver for Intel)

## Notes
- VAAPI performance varies by GPU generation
- Quality settings may need adjustment compared to software encoders
- Not all HandBrake filters work with hardware acceleration
- The test.mp4 file is automatically created during the build process