# start_stream_eigerclient.c

C version of `start_stream_eigerclient.py` - Configures EIGER detector for streaming data acquisition.

## Overview

This program provides a command-line interface to configure and control an EIGER detector for streaming data acquisition. It performs the same operations as the Python script `start_stream_eigerclient.py`, but implemented in C for Windows using the WinHTTP API.

## Features

- **Detector Configuration**: Configures all detector settings including:
  - Countrate correction, retrigger, counting mode
  - Virtual pixel correction, mask settings
  - Flatfield correction settings
  - Threshold configuration
  - Acquisition parameters (count_time, frame_time, nimages)
  - Data acquisition interfaces (monitor, filewriter, stream)

- **Workflow**: Follows the standard EIGER detector workflow:
  1. Connect to detector
  2. Disarm detector
  3. Initialize if needed
  4. Get detector status
  5. Configure all settings
  6. Arm detector
  7. Trigger acquisition
  8. Disarm detector

## Building

The program is automatically built as part of the CMake build system. On Windows, it will be compiled as `start_stream_eigerclient.exe` in the build output directory.

### Requirements

- Windows (uses WinHTTP API)
- CMake (version 3.11 or higher)
- Visual Studio or compatible C compiler

### Quick Build Instructions

```bash
cd documentation/stream_v2/examples
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

The executable will be created in `build/bin/start_stream_eigerclient.exe`.

### Detailed Build Instructions

See [COMPILE_start_stream_eigerclient.md](COMPILE_start_stream_eigerclient.md) for detailed compilation instructions, troubleshooting, and alternative build methods.

## Usage

### Basic Usage

```bash
start_stream_eigerclient.exe
```

This will use default values:
- IP: `172.31.1.1`
- Threshold: `15000` eV
- Number of images: `2000`
- Exposure time: `0.002` seconds
- Sleep time: `0.0` seconds

### Command-Line Options

```bash
start_stream_eigerclient.exe [options]
```

**Options:**

- `--ip IP` - Detector IP address (default: 172.31.1.1)
- `--threshold EV` - Energy threshold in eV (default: 15000)
- `--nimages N` - Number of images to capture (default: 2000)
- `--exposure SEC` - Exposure time per image in seconds (default: 0.002)
- `--sleep SEC` - Sleep time between frames in seconds (default: 0.0)
- `--force-init` - Force detector initialization even if already idle
- `--help` - Show help message

### Examples

**Configure detector with custom settings:**
```bash
start_stream_eigerclient.exe --ip 172.31.1.1 --threshold 45000 --nimages 6000 --exposure 0.001
```

**High-speed acquisition:**
```bash
start_stream_eigerclient.exe --threshold 15000 --nimages 10000 --exposure 0.0001
```

**Force initialization:**
```bash
start_stream_eigerclient.exe --ip 172.31.1.1 --force-init
```

## Configuration Details

The program configures the detector with the following default settings:

### Detector Settings
- `countrate_correction_applied`: false
- `retrigger`: false
- `counting_mode`: "normal"
- `virtual_pixel_correction_applied`: true
- `mask_to_zero`: true
- `test_image_mode`: ""
- `flatfield_correction_applied`: false

### Data Acquisition Interfaces
- `monitor`: disabled
- `filewriter`: disabled
- `stream`: enabled
  - `format`: "cbor"
  - `header_detail`: "all"

### Thresholds
- Threshold 1: Enabled (configured via `--threshold`)
- Threshold 2: Disabled

## Comparison with Python Version

This C implementation provides the same functionality as `start_stream_eigerclient.py`, but:
- **No dependencies**: Single executable, no Python runtime required
- **Faster startup**: Compiled code starts faster than Python scripts
- **Windows-only**: Currently implemented only for Windows (uses WinHTTP API)
- **Simpler**: Command-line only, no GUI

The Python version offers:
- Cross-platform support (Windows, Linux, macOS)
- More flexible configuration (JSON files, custom flatfield correction)
- Better error handling and logging
- Integration with Python ecosystem

## Notes

- The program uses HTTP port 80 by default for communication with the detector
- All HTTP requests use the EIGER REST API v1.0
- The program does not wait for acquisition to complete - it triggers and immediately disarms
- For Linux support, the code would need to be extended with libcurl or similar HTTP library

## Troubleshooting

**Connection errors:**
- Verify the detector IP address is correct
- Ensure the detector is powered on and accessible on the network
- Check firewall settings

**Configuration errors:**
- Verify the detector is in a valid state (idle or ready for initialization)
- Check that threshold values are within valid range for your detector model
- Ensure exposure time is reasonable for your detector

**Build errors:**
- Ensure WinHTTP library is available (included with Windows SDK)
- Verify CMake is properly configured for your compiler
