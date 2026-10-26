# D3D12 VPBlit LUT Sample

## Overview

`D3D12VPBlitLUT` is a command line tool that runs raw video frames through the
Direct3D 12 video processor and performs video processing with **1D and 3D LUT (Look-Up Table) transforms**, using the
[D3D12 Video Processing LUT Compound API](https://github.com/microsoft/DirectX-Specs/blob/master/d3d/D3D12_Video_Processing_LUT.md).


https://microsoft.github.io/DirectX-Specs/d3d/D3D12_Video_Processing_LUT.html

For every frame it:

1. Uploads a raw **P010** frame (1920 × 1080) into an input texture.
2. Calls `ProcessFrames2` once, which performs three operations in a single pass:
   * a **CSC** (colour space conversion) matrix — BT.2020 limited-range YCbCr → RGB,
   * a **1D LUT** (RGBA16) — a per-channel transfer curve,
   * a **3D LUT** (RGBA16, trilinear interpolation) — a full colour cube, for example an
     HDR-to-SDR conversion.
3. Reads the **NV12** result back and writes it to disk as a raw frame.

The CSC matrix and both tables are pushed into the video processor on the first frame
via `D3D12_VIDEO_PROCESS_LUT_UPDATE_FLAGS` and reused for every frame after that, which
is the intended usage pattern for a video playback pipeline.

This project is intended as a reference implementation for developers integrating advanced color processing.



The whole sample is one source file, `D3D12VPBlitLUT.cpp`, split into numbered
sections that follow the order the work happens in.

## Build Requirements

* Windows 10 or 11 with a **D3D12 compatible GPU**.
* A graphics driver that supports **D3D12 Video Processing with the LUT Compound API**.
  If yours does not, the sample tells you so and exits; see [Limitations](#limitations).
* **CMake 3.20** or newer.
* A **C++14** compiler: MSVC (Visual Studio 2019 or newer) or Clang. The Windows SDK
  supplies `d3d12.lib` and `dxgi.lib`.
* **D3D12 Agility SDK 1.620 or newer** — downloaded and wired up automatically, or taken from a
  local copy; see below.
* Internet access on the first configure, if the SDK is downloaded.
* Optional: the **Graphics Tools** optional Windows feature, for the D3D12 debug layer.

### The Agility SDK Dependency

The LUT Compound API is introduced in the Agility SDK v1.620.  `CMakeLists.txt` handles this dependency:

* Downloads a pinned `Microsoft.Direct3D.D3D12` release (`AGILITY_SDK_DEFAULT_VERSION` in
  `CMakeLists.txt`) from NuGet at configure time and checks it against the SHA-256 recorded
  next to it, unless `AGILITY_SDK_VERSION` selects another release or `AGILITY_SDK_ROOT`
  points at a local copy. The package's `D3D12Core.dll` runs inside the sample's process,
  which is why the download is verified,
* Puts its headers ahead of the Windows SDK headers on the include path,
* Copies `D3D12Core.dll` and `d3d12SDKLayers.dll` into a `D3D12` subfolder next to the
  built executable.

The runtime side is two exported symbols at the top of `D3D12VPBlitLUT.cpp`:

```cpp
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = D3D12_SDK_VERSION; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = u8".\\D3D12\\"; }
```

`D3D12_SDK_VERSION` comes from the Agility SDK's own `d3d12.h`, so the exported version
always matches the headers the sample was compiled against. It must also match the
`D3D12Core.dll` that gets staged, which comes from the same package. A preview runtime
reports `D3D12_PREVIEW_SDK_VERSION` instead, and device creation then fails with
`D3D12_ERROR_INVALID_REDIST` (`0x887E0003`).

To build against an SDK you already have already downloaded, point `AGILITY_SDK_ROOT` at the extracted package:

```
cmake -S . -B build -DAGILITY_SDK_ROOT=C:/path/to/Microsoft.Direct3D.D3D12.1.620.0
```

To build against a different release, set the version and its hash (from
`cmake -E sha256sum` on the package). Without a hash the download still works, but is not
verified, and configure prints a warning with the hash to pin:

```
cmake -S . -B build -DAGILITY_SDK_VERSION=1.620.1 -DAGILITY_SDK_SHA256=<sha256>
```

To use the newest stable (non-preview) release that is `1.620.0` or newer, set the version
to `latest`. The resolved version is remembered for the build tree; configure with
`cmake --fresh` (or use a new build directory) to pick up a newer release:

```
cmake -S . -B build -DAGILITY_SDK_VERSION=latest
```

## Building

```
cmake -S . -B build
cmake --build build --config Debug
```

That produces `build/Debug/D3D12VPBlitLUT.exe` with the Agility SDK redistributable and
the sample LUT tables staged alongside it, so it is ready to run in place. Use
`--config Release` for an optimised build.

Useful CMake options:

| Option | Default | Effect |
| ------ | ------- | ------ |
| `D3D12VPBLITLUT_WARNINGS_AS_ERRORS` | `OFF` | `/WX` on MSVC, `-Werror` on Clang. |
| `D3D12VPBLITLUT_BUILD_FUZZERS` | `OFF` | Also build the libFuzzer targets; see [Fuzzing](#fuzzing). |
| `AGILITY_SDK_VERSION` | the pinned release | NuGet package version to download, or `latest` for the newest stable release >= `1.620.0`. |
| `AGILITY_SDK_SHA256` | empty | Expected package hash. Empty uses the built-in hash for the pinned release, and skips verification for any other. Not allowed with `latest`. |
| `AGILITY_SDK_ROOT` | empty | Use an already extracted SDK instead of downloading. |

The build is warning-clean with MSVC `/W4` and with Clang
`-Wall -Wextra -Wpedantic` (minus the suppressions the Windows SDK headers require,
which `CMakeLists.txt` applies for you).

## Fuzzing

The `fuzz/` folder has two [libFuzzer](https://llvm.org/docs/LibFuzzer.html) targets for
the sample's input handling. Each one compiles `D3D12VPBlitLUT.cpp` itself, with `main()`
left out, so it exercises the sample's own code. Neither creates a D3D12 device, so no GPU
is needed.

| Target | Covers |
| ------ | ------ |
| `fuzz_command_line` | Command line parsing, then the 3D LUT size check and output file naming for every command line that parses. The input is one argument per line, in UTF-8. |
| `fuzz_input_files` | Reading the LUT files and the input video: size validation, frame counting and frame reads. |

The sample never looks at the contents of the video frames or LUT tables on the CPU; it
hands them to the GPU as they are. So `fuzz_input_files` covers how the files are
read, and how the driver handles odd frame or LUT data is out of its reach.

The targets build with MSVC or clang-cl, for x64. Release builds run fastest:

```
cmake -S . -B build -DD3D12VPBLITLUT_BUILD_FUZZERS=ON
cmake --build build --config Release --target fuzz_command_line fuzz_input_files
```

Run them from the build folder. libFuzzer adds new inputs to the first folder on the
command line, so keep that one in the build tree and pass the checked-in seeds after it:

```
cd build\fuzz
mkdir corpus\command_line corpus\input_files
Release\fuzz_command_line.exe -dict=..\..\fuzz\command_line.dict -max_total_time=600 ^
    corpus\command_line ..\..\fuzz\corpus\command_line
Release\fuzz_input_files.exe -max_total_time=600 corpus\input_files
```

Add `-close_fd_mask=2` to hide the sample's own error messages while fuzzing. A crash or
broken invariant saves the input as `crash-<hash>`. To reproduce it, pass that file as the
only argument, without `-close_fd_mask`, so the `Invariant violated:` message is shown.

## Command Line Usage

```
D3D12VPBlitLUT.exe -i <input_file>
                   -lut1d <1d_lut_file> -lut1dsize <size>
                   -lut3d <3d_lut_file> -lut3dsize <17|33|45|65>
                   [-o <output_base>] [-adapter <index>] [-debuglayer]
```

| Option | Required | Description |
| ------ | -------- | ----------- |
| `-i <file>` | yes | Raw P010 input, 1920 × 1080, frames back to back. |
| `-lut1d <file>` | yes | 1D LUT table, RGBA16, 8 bytes per entry. |
| `-lut1dsize <n>` | yes | 1D LUT entry count. Must equal the adapter's native 1D LUT size, which the sample prints at startup. |
| `-lut3d <file>` | yes | 3D LUT table, RGBA16, 8 bytes per entry, R varying fastest. |
| `-lut3dsize <n>` | yes | 3D LUT edge length: 17, 33, 45 or 65. |
| `-o <output_base>` | no | Output name stem. Defaults to `output_1920x1080`. |
| `-adapter <index>` | no | Adapter to use, by index as listed at startup. Defaults to the first hardware adapter. |
| `-debuglayer` | no | Enable the D3D12 debug layer. On by default in `Debug` and `RelWithDebInfo` builds. |

### Example

Run from the build output directory, where CMake has already placed the LUT tables:

```
D3D12VPBlitLUT.exe -i input_1920x1080_p010.bin ^
                   -lut1d lut1d_identity_1024.dat -lut1dsize 1024 ^
                   -lut3d lut3d_65x65x65.dat -lut3dsize 65 ^
                   -o out/result
```

This writes `out/result_frame_000.bin`, `out/result_frame_001.bin`, and so on — one raw
NV12 frame per file.

## Input and Output

### Input Video

Sample input video provided is input_1920x1080_p010.bin.

| Item | Value |
| ---- | ----- |
| Format | Raw **P010**, no container and no header |
| Resolution | 1920 × 1080 |
| Colour space | BT.2020 PQ, treated as `DXGI_COLOR_SPACE_CUSTOM` and converted by the CSC matrix in the sample |
| Layout | 16-bit luma plane, then a 16-bit interleaved chroma plane at half height |
| Frame size | `1920 × 1080 × 2` (Y) `+ 1920 × 1080` (UV) = 6,220,800 bytes |

The file is read as a sequence of complete frames; the frame count is just the file size
divided by the frame size, and any trailing partial frame is ignored.

### LUT tables

Two tables are included so you can run the sample immediately:

| File | Contents |
| ---- | -------- |
| `lut1d_identity_1024.dat` | 1024-entry identity 1D LUT, RGBA16 (8,192 bytes) |
| `lut3d_65x65x65.dat` | 65 × 65 × 65 HDR-to-SDR 3D LUT, RGBA16 (2,197,000 bytes) |

Both are raw RGBA16 with no header: four 16-bit unsigned channels per entry, alpha
unused. The 3D LUT is stored with the red axis varying fastest, then green, then blue.
The sample checks each file's size against the requested dimensions and against the
adapter's native 1D LUT size before uploading.

### Output Video

| Item | Value |
| ---- | ----- |
| Format | Raw **NV12**, tightly packed (row padding removed) |
| Resolution | 1920 × 1080 |
| Colour space | `DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709` |
| Naming | `<output_base>_frame_NNN.bin`, one file per frame |
| Frame size | 3,110,400 bytes |

To view a result:

```
ffplay -f rawvideo -pixel_format nv12 -video_size 1920x1080 out/result_frame_000.bin
```

## Limitations

* **Fixed resolution.** Input and output are hard-coded to 1920 × 1080. A different
  resolution needs the constants at the top of `D3D12VPBlitLUT.cpp` changed and
  rebuilt.
* **Fixed formats.** P010 in, NV12 out. The LUT Compound API is not limited to these —
  they are just what this sample demonstrates.



## Disclaimer

This sample is provided for demonstration and reference purposes. It is not production
code and will need work before being integrated into a real application.

## References

* [DirectX 12 Agility SDK](https://devblogs.microsoft.com/directx/directx12agility/)
* [Getting started with the DirectX 12 Agility SDK](https://devblogs.microsoft.com/directx/gettingstarted-dx12agility/)
* [`Microsoft.Direct3D.D3D12` on NuGet](https://www.nuget.org/packages/Microsoft.Direct3D.D3D12/)
* [D3D12 Video Processing LUT specification](https://github.com/microsoft/DirectX-Specs/blob/master/d3d/D3D12_Video_Processing_LUT.md)
* [Direct3D 12 video processing overview](https://learn.microsoft.com/en-us/windows/win32/medfound/direct3d-12-video-overview)
* [DXGI color space reference](https://learn.microsoft.com/en-us/windows/win32/api/dxgicommon/ne-dxgicommon-dxgi_color_space_type)
