/*
 * Copyright (C) 2026 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */


//=======================================================================================
// D3D12VPBlitLUT - video processing with CSC + 1D LUT + 3D LUT
//
// Reads raw P010 frames from a file, runs each one through the Direct3D 12 video
// processor using the LUT Compound API (YCbCr->RGB colour space conversion, then a 1D
// LUT, then a 3D LUT), and writes the NV12 results back to disk.
//
// The file is laid out in the order the work happens:
//
//   1. Agility SDK parameters      6. Capability checks
//   2. Error reporting             7. Video processor and command objects
//   3. Small RAII / struct helpers 8. Resource creation and LUT upload
//   4. File I/O                    9. Per-frame upload / process / readback / save
//   5. Command line parsing       10. Run() and main()
//
// See README.md for build instructions and the command line reference.
//=======================================================================================

// Windows and Direct3D.
#include <windows.h>

#include <comdef.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3d12video.h>
#include <dxgi1_6.h>
#include <dxgiformat.h>
#include <shellapi.h>
#include <wrl/client.h>

// C and C++ standard library.
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

//---------------------------------------------------------------------------------------
// 1. DirectX 12 Agility SDK parameters
//
// d3d12.dll looks for these two exports on the process' main module before the first
// D3D12 entry point runs, and loads D3D12Core.dll from D3D12SDKPath (relative to the
// executable). This is method 2a from
// https://devblogs.microsoft.com/directx/gettingstarted-dx12agility/#2.-set-agility-sdk-parameters
//
// The dependency is deliberately hard-coded: the LUT Compound API is not in the system
// D3D12 runtime, so there is no useful fallback. This build targets the 1.62 release
// candidate, whose D3D12Core.dll reports D3D12_SDK_VERSION (620). Do not use
// D3D12_PREVIEW_SDK_VERSION here: in the 1.62 headers that is 721, which does not match
// the RC D3D12Core.dll, and device creation fails with D3D12_ERROR_INVALID_REDIST.
//
// CMake stages the matching D3D12Core.dll and d3d12SDKLayers.dll into .\D3D12\ as a
// post-build step. The trailing backslash in D3D12SDKPath is required.
//
// Note that preview releases of the Agility SDK are only loaded when Windows Developer
// Mode is enabled; without it, device creation fails.
//---------------------------------------------------------------------------------------

extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = D3D12_SDK_VERSION; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = u8".\\D3D12\\"; }

namespace {

using Microsoft::WRL::ComPtr;

//---------------------------------------------------------------------------------------
// 2. Error reporting
//---------------------------------------------------------------------------------------

// Non-owning, purely for diagnostics: it lets ReportFailure() add the device removal
// reason to a failure message. Owned by Run(), and never used after Run() returns.
ID3D12Device* g_deviceForDiagnostics = nullptr;

void ReportFailure(const char* expression, HRESULT hr, int line)
{
    // The expression comes from the stringised source text, so it is always ASCII.
    const std::wstring text(expression, expression + std::strlen(expression));
    const _com_error error(hr);

    std::wcerr << L"FAILED at line " << std::dec << line << L": " << text << L" = 0x"
               << std::hex << static_cast<unsigned long>(hr) << std::dec << L" ("
               << error.ErrorMessage() << L")\n";

    if (g_deviceForDiagnostics != nullptr)
    {
        const HRESULT removedReason = g_deviceForDiagnostics->GetDeviceRemovedReason();
        if (FAILED(removedReason))
        {
            const _com_error removedError(removedReason);
            std::wcerr << L"  Device removed reason: 0x" << std::hex
                       << static_cast<unsigned long>(removedReason) << std::dec << L" ("
                       << removedError.ErrorMessage() << L")\n";
        }
    }
}

// Every fallible function in this sample returns an HRESULT so that failures propagate
// the same way whether they come from D3D12 or from the sample's own validation.
//
// Use RETURN_IF_FAILED for D3D12 and Win32 calls, where the HRESULT and the source
// location are the only diagnosis available. Use RETURN_IF_FAILED_QUIET when calling one
// of the sample's own functions: by then something has already been reported, at the
// exact call that failed, and a second line of propagation trace only adds noise.
#define RETURN_IF_FAILED(expression)                        \
    do                                                      \
    {                                                       \
        const HRESULT hr_ = (expression);                   \
        if (FAILED(hr_))                                    \
        {                                                   \
            ReportFailure(#expression, hr_, __LINE__);      \
            return hr_;                                     \
        }                                                   \
    } while (false)

#define RETURN_IF_FAILED_QUIET(expression)                  \
    do                                                      \
    {                                                       \
        const HRESULT hr_ = (expression);                   \
        if (FAILED(hr_))                                    \
        {                                                   \
            return hr_;                                     \
        }                                                   \
    } while (false)

//---------------------------------------------------------------------------------------
// 3. Small helpers
//---------------------------------------------------------------------------------------

template <typename T, std::size_t N>
constexpr UINT CountOf(const T (&)[N])
{
    return static_cast<UINT>(N);
}

// Stream configuration. The sample is fixed to 1080p P010 in and 1080p NV12 out.
constexpr UINT kInputStreamWidth = 1920;
constexpr UINT kInputStreamHeight = 1080;
constexpr UINT kOutputStreamWidth = 1920;
constexpr UINT kOutputStreamHeight = 1080;

constexpr DXGI_FORMAT kInputFormat = DXGI_FORMAT_P010;
constexpr DXGI_FORMAT kOutputFormat = DXGI_FORMAT_NV12;
constexpr DXGI_FORMAT kLutFormat = DXGI_FORMAT_R16G16B16A16_UNORM;

constexpr DXGI_COLOR_SPACE_TYPE kInputColorSpace = DXGI_COLOR_SPACE_CUSTOM;
constexpr DXGI_COLOR_SPACE_TYPE kOutputColorSpace = DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709;
constexpr DXGI_COLOR_SPACE_TYPE kUpsampledChromaColorSpace = DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020;

constexpr DXGI_RATIONAL kFrameRate = { 30, 1 };

// One RGBA16 LUT entry, in both the 1D and the 3D table.
constexpr UINT kLutEntryBytes = 8;

// P010 in a file: a 16-bit full resolution luma plane followed by a 16-bit interleaved
// chroma plane at half height.
constexpr UINT64 kInputFrameBytes =
    static_cast<UINT64>(kInputStreamWidth) * kInputStreamHeight * 2 +   // Y
    static_cast<UINT64>(kInputStreamWidth) * kInputStreamHeight;        // UV

// NV12 out: 8-bit luma plane plus an 8-bit interleaved chroma plane at half height.
constexpr UINT64 kOutputFrameBytes =
    static_cast<UINT64>(kOutputStreamWidth) * kOutputStreamHeight +       // Y
    static_cast<UINT64>(kOutputStreamWidth) * kOutputStreamHeight / 2;    // UV

// BT.2020 limited-range YCbCr to RGB, laid out as the D3D12 spec defines pCSCMatrix:
//   O_y = sum(x = 0..2) M[x][y] * I_x + M[3][y]
// x is the input channel (I_R, I_G, I_B), y the output channel (R, G, B) and row 3 the
// post-offset, all normalized (1.0 = full scale). The spec maps upsampled YUV input as
// V -> R, U -> G, Y -> B, so for P010 the input rows are in (Cr, Cb, Y) order.
//
// Limited-range input carries Y and Cb/Cr offset.
// Removing them before the matrix and folding that through it gives the
// post-offset, and normalized:
constexpr float kBt2020YuvToRgbMatrix[4][3] = {
    {   1.68522644042968750f,  -0.65296936035156250f,   0.00000000000000000f },  // Cr (I_R)
    {   0.00000000000000000f,  -0.188064575195312f,     2.150131225590000f   },  // Cb (I_G)
    {   1.16893005371093750f,   1.16893005371093750f,   1.16893005371093750f },  // Y  (I_B)
    {  -0.91567136230878530f,   0.34745884768199176f,  -1.14812371088191870f },  // post-offset
};

// Sentinel for "pick the first hardware adapter".
constexpr UINT kAutoSelectAdapter = 0xFFFFFFFFu;

class UniqueEvent
{
public:
    UniqueEvent() = default;

    ~UniqueEvent()
    {
        if (m_handle != nullptr)
        {
            ::CloseHandle(m_handle);
        }
    }

    UniqueEvent(const UniqueEvent&) = delete;
    UniqueEvent& operator=(const UniqueEvent&) = delete;

    HRESULT Create()
    {
        m_handle = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return m_handle != nullptr ? S_OK : HRESULT_FROM_WIN32(::GetLastError());
    }

    HANDLE Get() const { return m_handle; }

private:
    HANDLE m_handle = nullptr;
};

// Keeps a Map()/Unmap() pair balanced across early returns.
class MappedResource
{
public:
    MappedResource() = default;

    ~MappedResource()
    {
        if (m_resource != nullptr)
        {
            m_resource->Unmap(0, nullptr);
        }
    }

    MappedResource(const MappedResource&) = delete;
    MappedResource& operator=(const MappedResource&) = delete;

    HRESULT Map(ID3D12Resource* resource)
    {
        void* data = nullptr;
        RETURN_IF_FAILED(resource->Map(0, nullptr, &data));
        m_resource = resource;
        m_data = static_cast<BYTE*>(data);
        return S_OK;
    }

    BYTE* Data() const { return m_data; }

private:
    ID3D12Resource* m_resource = nullptr;
    BYTE* m_data = nullptr;
};

D3D12_HEAP_PROPERTIES MakeHeapProperties(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES properties = {};
    properties.Type = type;
    return properties;
}

D3D12_RESOURCE_DESC MakeBufferDesc(UINT64 bytes)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

D3D12_RESOURCE_DESC MakeTextureDesc(D3D12_RESOURCE_DIMENSION dimension,
                                    UINT64 width,
                                    UINT height,
                                    UINT16 depthOrArraySize,
                                    DXGI_FORMAT format,
                                    D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = dimension;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = depthOrArraySize;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    return desc;
}

D3D12_RESOURCE_BARRIER MakeTransitionBarrier(ID3D12Resource* resource,
                                             D3D12_RESOURCE_STATES before,
                                             D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

D3D12_TEXTURE_COPY_LOCATION MakeSubresourceLocation(ID3D12Resource* resource, UINT subresource)
{
    D3D12_TEXTURE_COPY_LOCATION location = {};
    location.pResource = resource;
    location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    location.SubresourceIndex = subresource;
    return location;
}

D3D12_TEXTURE_COPY_LOCATION MakeFootprintLocation(ID3D12Resource* resource,
                                                  const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint)
{
    D3D12_TEXTURE_COPY_LOCATION location = {};
    location.pResource = resource;
    location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    location.PlacedFootprint = footprint;
    return location;
}

// Copies rows from a tightly packed source into a destination with its own row pitch.
void CopyRows(BYTE* destination,
              UINT64 destinationRowPitch,
              const BYTE* source,
              UINT64 sourceRowPitch,
              UINT rowCount)
{
    const size_t bytesPerRow = static_cast<size_t>(std::min(sourceRowPitch, destinationRowPitch));

    for (UINT row = 0; row < rowCount; ++row)
    {
        std::memcpy(destination, source, bytesPerRow);
        destination += destinationRowPitch;
        source += sourceRowPitch;
    }
}

//---------------------------------------------------------------------------------------
// 4. File I/O
//---------------------------------------------------------------------------------------

HRESULT ReadWholeFile(const std::wstring& path, std::vector<BYTE>& contents)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        std::wcerr << L"Failed to open for reading: " << path << L"\n";
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    contents.resize(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(contents.data()), size))
    {
        std::wcerr << L"Failed to read " << path << L"\n";
        return E_FAIL;
    }

    return S_OK;
}

HRESULT WriteWholeFile(const std::wstring& path, const std::vector<BYTE>& contents)
{
    const size_t separator = path.find_last_of(L"\\/");
    if (separator != std::wstring::npos)
    {
        ::CreateDirectoryW(path.substr(0, separator).c_str(), nullptr);
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        std::wcerr << L"Failed to open for writing: " << path << L"\n";
        return E_FAIL;
    }

    file.write(reinterpret_cast<const char*>(contents.data()),
               static_cast<std::streamsize>(contents.size()));
    if (!file.good())
    {
        std::wcerr << L"Failed to write " << path << L"\n";
        return E_FAIL;
    }

    return S_OK;
}

// Keeps the input file open for the duration of the run so that the per-frame cost is a
// seek and a read rather than an open.
class FrameReader
{
public:
    HRESULT Open(const std::wstring& path, UINT64 frameBytes)
    {
        m_file.open(path, std::ios::binary | std::ios::ate);
        if (!m_file.is_open())
        {
            std::wcerr << L"Failed to open input file: " << path << L"\n";
            return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        }

        const UINT64 fileBytes = static_cast<UINT64>(m_file.tellg());
        m_frameBytes = frameBytes;
        m_frameCount = static_cast<UINT>(fileBytes / frameBytes);

        if (m_frameCount == 0)
        {
            std::wcerr << L"Input file holds no complete " << std::dec << kInputStreamWidth << L"x"
                       << kInputStreamHeight << L" P010 frame: expected at least " << frameBytes
                       << L" bytes, found " << fileBytes << L".\n";
            return E_INVALIDARG;
        }

        return S_OK;
    }

    UINT FrameCount() const { return m_frameCount; }

    HRESULT ReadFrame(UINT index, std::vector<BYTE>& frame)
    {
        frame.resize(static_cast<size_t>(m_frameBytes));

        m_file.seekg(static_cast<std::streamoff>(index * m_frameBytes), std::ios::beg);
        if (!m_file.read(reinterpret_cast<char*>(frame.data()),
                         static_cast<std::streamsize>(m_frameBytes)))
        {
            std::wcerr << L"Failed to read frame " << std::dec << index << L"\n";
            return E_FAIL;
        }

        return S_OK;
    }

private:
    std::ifstream m_file;
    UINT64 m_frameBytes = 0;
    UINT m_frameCount = 0;
};

//---------------------------------------------------------------------------------------
// 5. Command line parsing
//---------------------------------------------------------------------------------------

struct Options
{
    std::wstring inputPath;
    std::wstring outputPathBase;
    std::wstring lut1dPath;
    std::wstring lut3dPath;
    UINT lut1dSize = 0;
    UINT lut3dSize = 0;
    UINT adapterIndex = kAutoSelectAdapter;
    bool enableDebugLayer = false;
};

// Windows hands main() its arguments in the active code page, which cannot represent
// every valid path, so the command line is re-fetched as UTF-16 instead.
class CommandLine
{
public:
    CommandLine()
    {
        m_argv = ::CommandLineToArgvW(::GetCommandLineW(), &m_argc);
        if (m_argv == nullptr)
        {
            m_argc = 0;
        }
    }

    ~CommandLine()
    {
        if (m_argv != nullptr)
        {
            ::LocalFree(m_argv);
        }
    }

    CommandLine(const CommandLine&) = delete;
    CommandLine& operator=(const CommandLine&) = delete;

    int Count() const { return m_argc; }
    const wchar_t* operator[](int index) const { return m_argv[index]; }

private:
    int m_argc = 0;
    wchar_t** m_argv = nullptr;
};

void PrintUsage()
{
    std::wcout
        << L"\n"
           L"D3D12VPBlitLUT - D3D12 video processing with CSC + 1D LUT + 3D LUT\n"
           L"\n"
           L"Usage:\n"
           L"  D3D12VPBlitLUT.exe -i <input.bin> -lut1d <file> -lut1dsize <n>\n"
           L"                     -lut3d <file> -lut3dsize <17|33|45|65>\n"
           L"                     [-o <output_base>] [-adapter <index>] [-debuglayer]\n"
           L"\n"
           L"Required:\n"
           L"  -i <file>            Raw P010 input, 1920x1080, one frame after another.\n"
           L"  -lut1d <file>        1D LUT table, RGBA16 (8 bytes per entry).\n"
           L"  -lut1dsize <n>       1D LUT entry count; must match the hardware's native size.\n"
           L"  -lut3d <file>        3D LUT table, RGBA16 (8 bytes per entry), R fastest.\n"
           L"  -lut3dsize <n>       3D LUT edge length: 17, 33, 45 or 65.\n"
           L"\n"
           L"Optional:\n"
           L"  -o <output_base>     Output name stem. Default: output_1920x1080\n"
           L"                       Frames are written as <base>_frame_NNN.bin\n"
           L"  -adapter <index>     Adapter to use, as listed at startup. Default: the first\n"
           L"                       hardware adapter.\n"
           L"  -debuglayer          Enable the D3D12 debug layer (on by default in Debug\n"
           L"                       builds).\n"
           L"\n"
           L"Example:\n"
           L"  D3D12VPBlitLUT.exe -i input_1080p_p010.bin ^\n"
           L"                     -lut1d lut1d_identity_1024.dat -lut1dsize 1024 ^\n"
           L"                     -lut3d lut3d_65x65x65.dat -lut3dsize 65 ^\n"
           L"                     -o out/result\n"
           L"\n";
}

bool ParseUInt(const wchar_t* text, UINT& value)
{
    wchar_t* end = nullptr;
    const unsigned long parsed = std::wcstoul(text, &end, 10);
    if (end == text || *end != L'\0')
    {
        return false;
    }

    value = static_cast<UINT>(parsed);
    return true;
}

HRESULT ParseCommandLine(const CommandLine& commandLine, Options& options)
{
    for (int i = 1; i < commandLine.Count(); ++i)
    {
        const wchar_t* argument = commandLine[i];
        const bool hasValue = (i + 1) < commandLine.Count();

        if (std::wcscmp(argument, L"-debuglayer") == 0)
        {
            options.enableDebugLayer = true;
            continue;
        }

        if (std::wcscmp(argument, L"-h") == 0 || std::wcscmp(argument, L"-?") == 0 ||
            std::wcscmp(argument, L"--help") == 0)
        {
            return E_INVALIDARG;
        }

        if (!hasValue)
        {
            std::wcerr << L"Missing value for " << argument << L"\n";
            return E_INVALIDARG;
        }

        const wchar_t* value = commandLine[++i];
        bool parsed = true;

        if (std::wcscmp(argument, L"-i") == 0)
        {
            options.inputPath = value;
        }
        else if (std::wcscmp(argument, L"-o") == 0)
        {
            options.outputPathBase = value;
        }
        else if (std::wcscmp(argument, L"-lut1d") == 0)
        {
            options.lut1dPath = value;
        }
        else if (std::wcscmp(argument, L"-lut3d") == 0)
        {
            options.lut3dPath = value;
        }
        else if (std::wcscmp(argument, L"-lut1dsize") == 0)
        {
            parsed = ParseUInt(value, options.lut1dSize);
        }
        else if (std::wcscmp(argument, L"-lut3dsize") == 0)
        {
            parsed = ParseUInt(value, options.lut3dSize);
        }
        else if (std::wcscmp(argument, L"-adapter") == 0)
        {
            parsed = ParseUInt(value, options.adapterIndex);
        }
        else
        {
            std::wcerr << L"Unknown option: " << argument << L"\n";
            return E_INVALIDARG;
        }

        if (!parsed)
        {
            std::wcerr << L"Invalid value for " << argument << L": " << value << L"\n";
            return E_INVALIDARG;
        }
    }

    if (options.inputPath.empty() || options.lut1dPath.empty() || options.lut3dPath.empty() ||
        options.lut1dSize == 0 || options.lut3dSize == 0)
    {
        return E_INVALIDARG;
    }

    if (options.outputPathBase.empty())
    {
        std::wostringstream stream;
        stream << L"output_" << kOutputStreamWidth << L"x" << kOutputStreamHeight;
        options.outputPathBase = stream.str();
    }

    return S_OK;
}

std::wstring MakeOutputPath(const std::wstring& base, UINT frameIndex)
{
    const size_t extension = base.find_last_of(L'.');

    std::wostringstream stream;
    stream << (extension == std::wstring::npos ? base : base.substr(0, extension))
           << L"_frame_" << std::setfill(L'0') << std::setw(3) << frameIndex << L".bin";
    return stream.str();
}

//---------------------------------------------------------------------------------------
// 6. Device and capability checks
//---------------------------------------------------------------------------------------

void EnableDebugLayer()
{
    ComPtr<ID3D12Debug> debug;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
    {
        std::wcerr << L"Warning: the D3D12 debug layer is unavailable. Install the "
                      L"\"Graphics Tools\" optional Windows feature to use it.\n";
        return;
    }

    debug->EnableDebugLayer();
    std::wcout << L"D3D12 debug layer enabled.\n";
}

HRESULT SelectAdapter(IDXGIFactory4* factory, UINT requestedIndex, ComPtr<IDXGIAdapter1>& selected)
{
    std::wcout << L"Available adapters:\n";

    ComPtr<IDXGIAdapter1> candidate;
    UINT selectedIndex = kAutoSelectAdapter;

    for (UINT index = 0; factory->EnumAdapters1(index, candidate.ReleaseAndGetAddressOf()) !=
                         DXGI_ERROR_NOT_FOUND;
         ++index)
    {
        DXGI_ADAPTER_DESC1 desc = {};
        RETURN_IF_FAILED(candidate->GetDesc1(&desc));

        const bool isSoftware = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;

        std::wcout << L"  [" << std::dec << index << L"] " << desc.Description
                   << (isSoftware ? L" (software)" : L"") << L"\n";

        const bool wanted = (requestedIndex == kAutoSelectAdapter) ? !isSoftware
                                                                   : (index == requestedIndex);
        if (wanted && selected == nullptr)
        {
            selected = candidate;
            selectedIndex = index;
        }
    }

    if (selected == nullptr)
    {
        if (requestedIndex == kAutoSelectAdapter)
        {
            std::wcerr << L"No hardware adapter found.\n";
        }
        else
        {
            std::wcerr << L"Adapter index " << std::dec << requestedIndex
                       << L" does not exist.\n";
        }
        return E_INVALIDARG;
    }

    std::wcout << L"Using adapter [" << std::dec << selectedIndex << L"].\n";
    return S_OK;
}

HRESULT CreateDevice(const Options& options, ComPtr<ID3D12Device>& device)
{
    ComPtr<IDXGIFactory4> factory;
    RETURN_IF_FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));

    ComPtr<IDXGIAdapter1> adapter;
    RETURN_IF_FAILED_QUIET(SelectAdapter(factory.Get(), options.adapterIndex, adapter));

    static const D3D_FEATURE_LEVEL kFeatureLevels[] = {
        D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
    };

    HRESULT hr = E_FAIL;
    for (const D3D_FEATURE_LEVEL level : kFeatureLevels)
    {
        hr = D3D12CreateDevice(adapter.Get(), level, IID_PPV_ARGS(&device));
        if (SUCCEEDED(hr))
        {
            std::wcout << L"Created D3D12 device at feature level 0x" << std::hex
                       << static_cast<unsigned>(level) << std::dec << L".\n";
            break;
        }
    }
    RETURN_IF_FAILED(hr);

    return S_OK;
}

// The 3D LUT edge length appears twice in the API: as a capability bit when querying
// support, and as an enum when configuring the transform.
struct Lut3DParameters
{
    D3D12_VIDEO_PROCESS_3DLUT_TABLE_DIMENSION dimension;
    D3D12_VIDEO_PROCESS_3DLUT_TABLE_DIMENSION_SUPPORT_FLAGS supportFlag;
    D3D12_VIDEO_PROCESS_3DLUT_INTERPOLATION_MODE interpolationMode;
    D3D12_VIDEO_PROCESS_3DLUT_INTERPOLATION_MODE_SUPPORT_FLAGS interpolationModeFlag;
};

HRESULT Resolve3DLutParameters(UINT edgeLength, Lut3DParameters& resolved)
{
    resolved.interpolationMode = D3D12_VIDEO_PROCESS_3DLUT_INTERPOLATION_MODE_TRILINEAR;
    resolved.interpolationModeFlag = D3D12_VIDEO_PROCESS_3DLUT_INTERPOLATION_MODE_SUPPORT_FLAG_TRILINEAR;

    switch (edgeLength)
    {
    case 17:
        resolved.dimension = D3D12_VIDEO_PROCESS_3DLUT_TABLE_DIMENSION_17x17x17;
        resolved.supportFlag = D3D12_VIDEO_PROCESS_3DLUT_TABLE_DIMENSION_SUPPORT_FLAG_17x17x17;
        return S_OK;
    case 33:
        resolved.dimension = D3D12_VIDEO_PROCESS_3DLUT_TABLE_DIMENSION_33x33x33;
        resolved.supportFlag = D3D12_VIDEO_PROCESS_3DLUT_TABLE_DIMENSION_SUPPORT_FLAG_33x33x33;
        return S_OK;
    case 45:
        resolved.dimension = D3D12_VIDEO_PROCESS_3DLUT_TABLE_DIMENSION_45x45x45;
        resolved.supportFlag = D3D12_VIDEO_PROCESS_3DLUT_TABLE_DIMENSION_SUPPORT_FLAG_45x45x45;
        return S_OK;
    case 65:
        resolved.dimension = D3D12_VIDEO_PROCESS_3DLUT_TABLE_DIMENSION_65x65x65;
        resolved.supportFlag = D3D12_VIDEO_PROCESS_3DLUT_TABLE_DIMENSION_SUPPORT_FLAG_65x65x65;
        return S_OK;
    default:
        std::wcerr << L"Unsupported 3D LUT size " << std::dec << edgeLength
                   << L"; valid values are 17, 33, 45 and 65.\n";
        return E_INVALIDARG;
    }
}

// Spells out a flags value as "FLAG_A | FLAG_B", for logging what the driver reported.
// Bits that aren't in the table are appended in hex rather than silently dropped.
struct FlagName
{
    UINT value;
    const wchar_t* name;
};

#define FLAG_NAME(flag) { static_cast<UINT>(flag), L#flag }

template <std::size_t N>
std::wstring FlagsToString(UINT flags, const FlagName (&names)[N], const wchar_t* noneName)
{
    if (flags == 0)
    {
        return noneName;
    }

    std::wostringstream text;
    UINT remaining = flags;
    for (const FlagName& entry : names)
    {
        if (entry.value != 0 && (flags & entry.value) == entry.value)
        {
            if (remaining != flags)
            {
                text << L" | ";
            }
            text << entry.name;
            remaining &= ~entry.value;
        }
    }

    if (remaining != 0)
    {
        if (remaining != flags)
        {
            text << L" | ";
        }
        text << L"0x" << std::hex << remaining;
    }
    return text.str();
}

std::wstring LutSupportFlagsToString(D3D12_VIDEO_PROCESS_LUT_SUPPORT_FLAGS flags)
{
    static const FlagName names[] = {
        FLAG_NAME(D3D12_VIDEO_PROCESS_LUT_SUPPORT_FLAG_SUPPORTED),
        FLAG_NAME(D3D12_VIDEO_PROCESS_LUT_SUPPORT_FLAG_1DLUT_SPACING_CURVE),
        FLAG_NAME(D3D12_VIDEO_PROCESS_LUT_SUPPORT_FLAG_1DLUT_NON_MONOTONICALLY_INCREASING_CURVE),
    };
    return FlagsToString(static_cast<UINT>(flags), names,
                         L"D3D12_VIDEO_PROCESS_LUT_SUPPORT_FLAG_NONE");
}

std::wstring Lut3DDimensionFlagsToString(D3D12_VIDEO_PROCESS_3DLUT_TABLE_DIMENSION_SUPPORT_FLAGS flags)
{
    static const FlagName names[] = {
        FLAG_NAME(D3D12_VIDEO_PROCESS_3DLUT_TABLE_DIMENSION_SUPPORT_FLAG_17x17x17),
        FLAG_NAME(D3D12_VIDEO_PROCESS_3DLUT_TABLE_DIMENSION_SUPPORT_FLAG_33x33x33),
        FLAG_NAME(D3D12_VIDEO_PROCESS_3DLUT_TABLE_DIMENSION_SUPPORT_FLAG_45x45x45),
        FLAG_NAME(D3D12_VIDEO_PROCESS_3DLUT_TABLE_DIMENSION_SUPPORT_FLAG_65x65x65),
    };
    return FlagsToString(static_cast<UINT>(flags), names,
                         L"D3D12_VIDEO_PROCESS_3DLUT_TABLE_DIMENSION_SUPPORT_FLAG_NONE");
}

std::wstring Lut3DInterpolationFlagsToString(D3D12_VIDEO_PROCESS_3DLUT_INTERPOLATION_MODE_SUPPORT_FLAGS flags)
{
    static const FlagName names[] = {
        FLAG_NAME(D3D12_VIDEO_PROCESS_3DLUT_INTERPOLATION_MODE_SUPPORT_FLAG_TRILINEAR),
        FLAG_NAME(D3D12_VIDEO_PROCESS_3DLUT_INTERPOLATION_MODE_SUPPORT_FLAG_TETRAHEDRAL),
    };
    return FlagsToString(static_cast<UINT>(flags), names,
                         L"D3D12_VIDEO_PROCESS_3DLUT_INTERPOLATION_MODE_SUPPORT_FLAG_NONE");
}

#undef FLAG_NAME

HRESULT CheckVideoProcessSupport(ID3D12VideoDevice5* videoDevice,
                                 const Options& options,
                                 const Lut3DParameters& lut3d)
{
    D3D12_FEATURE_DATA_VIDEO_PROCESS_SUPPORT1 support = {};
    support.NodeIndex = 0;
    support.InputSample.Width = kInputStreamWidth;
    support.InputSample.Height = kInputStreamHeight;
    support.InputSample.Format.Format = kInputFormat;
    support.InputSample.Format.ColorSpace = kInputColorSpace;
    support.InputFieldType = D3D12_VIDEO_FIELD_TYPE_NONE;
    support.InputStereoFormat = D3D12_VIDEO_FRAME_STEREO_FORMAT_NONE;
    support.InputFrameRate = kFrameRate;
    support.OutputFormat.Format = kOutputFormat;
    support.OutputFormat.ColorSpace = kOutputColorSpace;
    support.OutputStereoFormat = D3D12_VIDEO_FRAME_STEREO_FORMAT_NONE;
    support.OutputFrameRate = kFrameRate;
    support.SupportLUT.Format1DLUT = kLutFormat;
    support.SupportLUT.Format3DLUT = kLutFormat;
    support.SupportLUT.ColorSpace3DLUTOutput = kOutputColorSpace;
    //support.SupportLUT.Dimension3DLUTFlags = lut3d.supportFlag;
    //supportData.SupportLUT.Interpolation3DLUTFlags = lut3d.interpolationModeFlag;

    RETURN_IF_FAILED(videoDevice->CheckFeatureSupport(D3D12_FEATURE_VIDEO_PROCESS_SUPPORT1,
                                                      &support, sizeof(support)));

    // Dump what the driver reported, so a failed check below can be diagnosed.
    std::wcout << L"LUT support reported by the adapter:\n"
               << L"  SupportLUT.Flags                   = "
               << LutSupportFlagsToString(support.SupportLUT.Flags) << L"\n"
               << L"  SupportLUT.Native1DLUTSize         = " << std::dec
               << support.SupportLUT.Native1DLUTSize << L"\n"
               << L"  SupportLUT.Dimension3DLUTFlags     = "
               << Lut3DDimensionFlagsToString(support.SupportLUT.Dimension3DLUTFlags) << L"\n"
               << L"  SupportLUT.Interpolation3DLUTFlags = "
               << Lut3DInterpolationFlagsToString(support.SupportLUT.Interpolation3DLUTFlags)
               << L"\n";

    if ((support.SupportFlags & D3D12_VIDEO_PROCESS_SUPPORT_FLAG_SUPPORTED) == 0)
    {
        std::wcerr << L"This adapter does not support 1080p P010 -> NV12 video processing.\n";
        return DXGI_ERROR_UNSUPPORTED;
    }

    if (support.SupportLUT.Flags == D3D12_VIDEO_PROCESS_LUT_SUPPORT_FLAG_NONE)
    {
        std::wcerr << L"This adapter does not support the LUT transform.\n";
        return DXGI_ERROR_UNSUPPORTED;
    }

    //  Check 1DLUT Size
    if (options.lut1dSize != support.SupportLUT.Native1DLUTSize)
    {
        std::wcerr << L"Requested 1D LUT size " << std::dec << options.lut1dSize
                   << L" does not match this adapter's native size "
                   << support.SupportLUT.Native1DLUTSize << L". Pass -lut1dsize "
                   << support.SupportLUT.Native1DLUTSize << L" with a matching table.\n";
        return DXGI_ERROR_UNSUPPORTED;
    }

    // Check 3DLUT Size
    if (!(lut3d.supportFlag & support.SupportLUT.Dimension3DLUTFlags))
    {
        std::wcerr << L"Requested 3D LUT dimension " << std::dec << options.lut3dSize
            << L" is not supported.\n";
        return DXGI_ERROR_UNSUPPORTED;
    }

    // Check 3DLUT Interpolation Mode
    if (!(lut3d.interpolationModeFlag & support.SupportLUT.Interpolation3DLUTFlags))
    {
        std::wcerr << L"Requested 3D LUT interpolation mode " << "TRILINEAR" 
            << L" is not supported.\n";
        return DXGI_ERROR_UNSUPPORTED;
    }

    std::wcout << L"Video processing with 1D + 3D LUT is supported (native 1D LUT size "
               << std::dec << support.SupportLUT.Native1DLUTSize << L").\n";
    return S_OK;
}

//---------------------------------------------------------------------------------------
// 7. Video processor and command objects
//---------------------------------------------------------------------------------------

D3D12_VIDEO_PROCESS_LUT_TRANSFORM_CONFIGURATION MakeLutConfiguration(const Lut3DParameters& lut3d)
{
    D3D12_VIDEO_PROCESS_LUT_TRANSFORM_CONFIGURATION configuration = {};
    configuration.ConfigFlags = D3D12_VIDEO_PROCESS_LUT_TRANSFORM_CONFIGURATION_FLAG_ENABLE;
    configuration.UpsampledChromaInput = kUpsampledChromaColorSpace;
    configuration.Format1DLUT = kLutFormat;
    configuration.Format3DLUT = kLutFormat;
    configuration.ColorSpace3DLUTOutput = kOutputColorSpace;
    configuration.Interpolation3DLUT = lut3d.interpolationMode;
    configuration.Dimension3DLUT = lut3d.dimension;
    return configuration;
}

D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC1 MakeInputStreamDesc(
    const D3D12_VIDEO_PROCESS_LUT_TRANSFORM_CONFIGURATION& lutConfiguration)
{
    D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC1 desc = {};
    desc.Format = kInputFormat;
    desc.ColorSpace = kInputColorSpace;
    desc.SourceAspectRatio = { 1, 1 };
    desc.DestinationAspectRatio = { 1, 1 };
    desc.FrameRate = kFrameRate;
    desc.SourceSizeRange = { kInputStreamWidth, kInputStreamHeight, kInputStreamWidth, kInputStreamHeight };
    desc.DestinationSizeRange = { kOutputStreamWidth, kOutputStreamHeight, kOutputStreamWidth, kOutputStreamHeight };
    desc.EnableOrientation = FALSE;
    desc.FilterFlags = D3D12_VIDEO_PROCESS_FILTER_FLAG_NONE;
    desc.StereoFormat = D3D12_VIDEO_FRAME_STEREO_FORMAT_NONE;
    desc.FieldType = D3D12_VIDEO_FIELD_TYPE_NONE;
    desc.DeinterlaceMode = D3D12_VIDEO_PROCESS_DEINTERLACE_FLAG_NONE;
    desc.EnableAlphaBlending = FALSE;
    desc.NumPastFrames = 0;
    desc.NumFutureFrames = 0;
    desc.EnableAutoProcessing = FALSE;
    desc.ConfigurationLUT = lutConfiguration;
    return desc;
}

D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC MakeOutputStreamDesc()
{
    D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC desc = {};
    desc.Format = kOutputFormat;
    desc.ColorSpace = kOutputColorSpace;
    desc.FrameRate = kFrameRate;
    return desc;
}

// The sample never overlaps frames, so a single fence and a single CPU wait after each
// submission is enough to keep the two queues in order.
struct CommandContext
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12VideoDevice5> videoDevice;

    ComPtr<ID3D12CommandQueue> videoQueue;
    ComPtr<ID3D12CommandAllocator> videoAllocator;
    ComPtr<ID3D12VideoProcessCommandList3> videoList;
    ComPtr<ID3D12VideoProcessCommandList5> videoListLut;

    ComPtr<ID3D12CommandQueue> copyQueue;
    ComPtr<ID3D12CommandAllocator> copyAllocator;
    ComPtr<ID3D12GraphicsCommandList> copyList;

    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue = 0;
    UniqueEvent fenceEvent;

    HRESULT ExecuteAndWait(ID3D12CommandQueue* queue, ID3D12CommandList* list);
};

HRESULT CommandContext::ExecuteAndWait(ID3D12CommandQueue* queue, ID3D12CommandList* list)
{
    ID3D12CommandList* lists[] = { list };
    queue->ExecuteCommandLists(CountOf(lists), lists);

    ++fenceValue;
    RETURN_IF_FAILED(queue->Signal(fence.Get(), fenceValue));

    if (fence->GetCompletedValue() < fenceValue)
    {
        RETURN_IF_FAILED(fence->SetEventOnCompletion(fenceValue, fenceEvent.Get()));
        if (::WaitForSingleObject(fenceEvent.Get(), INFINITE) != WAIT_OBJECT_0)
        {
            return HRESULT_FROM_WIN32(::GetLastError());
        }
    }

    return S_OK;
}

HRESULT CreateCommandContext(const ComPtr<ID3D12Device>& device, CommandContext& context)
{
    context.device = device;
    RETURN_IF_FAILED(device->QueryInterface(IID_PPV_ARGS(&context.videoDevice)));

    {
        D3D12_COMMAND_QUEUE_DESC desc = {};
        desc.Type = D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS;
        RETURN_IF_FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&context.videoQueue)));

        desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        RETURN_IF_FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&context.copyQueue)));
    }

    RETURN_IF_FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS,
                                                    IID_PPV_ARGS(&context.videoAllocator)));
    RETURN_IF_FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS,
                                               context.videoAllocator.Get(), nullptr,
                                               IID_PPV_ARGS(&context.videoList)));
    RETURN_IF_FAILED(context.videoList->QueryInterface(IID_PPV_ARGS(&context.videoListLut)));
    RETURN_IF_FAILED(context.videoList->Close());

    RETURN_IF_FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                                                    IID_PPV_ARGS(&context.copyAllocator)));
    RETURN_IF_FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY,
                                               context.copyAllocator.Get(), nullptr,
                                               IID_PPV_ARGS(&context.copyList)));
    RETURN_IF_FAILED(context.copyList->Close());

    RETURN_IF_FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&context.fence)));
    RETURN_IF_FAILED(context.fenceEvent.Create());

    return S_OK;
}

HRESULT CreateVideoProcessor(ID3D12VideoDevice5* videoDevice,
                             const D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC1& inputStreamDesc,
                             const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC& outputStreamDesc,
                             ComPtr<ID3D12VideoProcessor2>& processor)
{
    {
        D3D12_FEATURE_DATA_VIDEO_PROCESSOR_SIZE2 processorSize = {};
        processorSize.NodeMask = 0;
        processorSize.pOutputStreamDesc = &outputStreamDesc;
        processorSize.NumInputStreamDescs = 1;
        processorSize.pInputStreamDescs = &inputStreamDesc;
        RETURN_IF_FAILED(videoDevice->CheckFeatureSupport(D3D12_FEATURE_VIDEO_PROCESSOR_SIZE2,
                                                          &processorSize, sizeof(processorSize)));
    }

    RETURN_IF_FAILED(videoDevice->CreateVideoProcessor2(0, &outputStreamDesc, 1, &inputStreamDesc,
                                                        nullptr, IID_PPV_ARGS(&processor)));
    return S_OK;
}

//---------------------------------------------------------------------------------------
// 8. Resource creation and LUT upload
//---------------------------------------------------------------------------------------

struct Resources
{
    ComPtr<ID3D12Resource> input;           // P010 texture, video process source
    ComPtr<ID3D12Resource> output;          // NV12 texture, video process target
    ComPtr<ID3D12Resource> lut1d;           // 1D LUT table
    ComPtr<ID3D12Resource> lut3d;           // 3D LUT table

    ComPtr<ID3D12Resource> inputUpload;     // staging for one P010 frame
    ComPtr<ID3D12Resource> outputReadback;  // staging for one NV12 frame

    // Copy layouts, queried once up front: plane 0 is luma, plane 1 is chroma.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT inputLayout[2] = {};
    UINT inputRowCount[2] = {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT outputLayout[2] = {};
};

HRESULT CreateStreamResources(ID3D12Device* device, Resources& resources)
{
    const D3D12_HEAP_PROPERTIES defaultHeap = MakeHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_HEAP_PROPERTIES uploadHeap = MakeHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_HEAP_PROPERTIES readbackHeap = MakeHeapProperties(D3D12_HEAP_TYPE_READBACK);

    const D3D12_RESOURCE_DESC inputDesc =
        MakeTextureDesc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, kInputStreamWidth, kInputStreamHeight, 1,
                        kInputFormat, D3D12_RESOURCE_FLAG_NONE);
    const D3D12_RESOURCE_DESC outputDesc =
        MakeTextureDesc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, kOutputStreamWidth, kOutputStreamHeight, 1,
                        kOutputFormat, D3D12_RESOURCE_FLAG_NONE);

    RETURN_IF_FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &inputDesc,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&resources.input)));
    RETURN_IF_FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
                                                     &outputDesc, D3D12_RESOURCE_STATE_COMMON,
                                                     nullptr, IID_PPV_ARGS(&resources.output)));

    UINT64 uploadBytes = 0;
    device->GetCopyableFootprints(&inputDesc, 0, CountOf(resources.inputLayout), 0,
                                  resources.inputLayout, resources.inputRowCount, nullptr,
                                  &uploadBytes);

    UINT64 readbackBytes = 0;
    device->GetCopyableFootprints(&outputDesc, 0, CountOf(resources.outputLayout), 0,
                                  resources.outputLayout, nullptr, nullptr, &readbackBytes);

    const D3D12_RESOURCE_DESC uploadDesc = MakeBufferDesc(uploadBytes);
    RETURN_IF_FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&resources.inputUpload)));

    const D3D12_RESOURCE_DESC readbackDesc = MakeBufferDesc(readbackBytes);
    RETURN_IF_FAILED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE,
                                                     &readbackDesc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&resources.outputReadback)));

    return S_OK;
}

HRESULT CreateLutResources(ID3D12Device* device, const Options& options, Resources& resources)
{
    const D3D12_HEAP_PROPERTIES defaultHeap = MakeHeapProperties(D3D12_HEAP_TYPE_DEFAULT);

    const D3D12_RESOURCE_DESC lut1dDesc =
        MakeTextureDesc(D3D12_RESOURCE_DIMENSION_TEXTURE1D, options.lut1dSize, 1, 1, kLutFormat,
                        D3D12_RESOURCE_FLAG_VIDEO_PROCESS_1DLUT_ONLY);
    RETURN_IF_FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &lut1dDesc,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&resources.lut1d)));

    const D3D12_RESOURCE_DESC lut3dDesc = MakeTextureDesc(
        D3D12_RESOURCE_DIMENSION_TEXTURE3D, options.lut3dSize, options.lut3dSize,
        static_cast<UINT16>(options.lut3dSize), kLutFormat,
        D3D12_RESOURCE_FLAG_VIDEO_PROCESS_3DLUT_ONLY);
    RETURN_IF_FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &lut3dDesc,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&resources.lut3d)));

    return S_OK;
}

// Reads both LUT files, validates their sizes against the requested dimensions, and
// copies them into their textures. The staging buffers and the file contents only live
// for the duration of this call, which also waits for the copy to complete.
HRESULT UploadLutTables(CommandContext& context, const Options& options, const Resources& resources)
{
    ID3D12Device* device = context.device.Get();
    const D3D12_HEAP_PROPERTIES uploadHeap = MakeHeapProperties(D3D12_HEAP_TYPE_UPLOAD);

    ComPtr<ID3D12Resource> lut1dUpload;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT lut1dLayout = {};
    {
        std::vector<BYTE> table;
        RETURN_IF_FAILED_QUIET(ReadWholeFile(options.lut1dPath, table));

        const size_t expectedBytes = static_cast<size_t>(options.lut1dSize) * kLutEntryBytes;
        if (table.size() != expectedBytes)
        {
            std::wcerr << L"1D LUT size mismatch: expected " << std::dec << expectedBytes
                       << L" bytes for " << options.lut1dSize << L" RGBA16 entries, got "
                       << table.size() << L".\n";
            return E_INVALIDARG;
        }

        const D3D12_RESOURCE_DESC lut1dDesc = resources.lut1d->GetDesc();
        UINT64 stagingBytes = 0;
        device->GetCopyableFootprints(&lut1dDesc, 0, 1, 0, &lut1dLayout, nullptr, nullptr,
                                      &stagingBytes);

        const D3D12_RESOURCE_DESC stagingDesc = MakeBufferDesc(stagingBytes);
        RETURN_IF_FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
                                                         &stagingDesc,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&lut1dUpload)));

        MappedResource mapped;
        RETURN_IF_FAILED_QUIET(mapped.Map(lut1dUpload.Get()));
        std::memcpy(mapped.Data() + lut1dLayout.Offset, table.data(), table.size());
    }

    ComPtr<ID3D12Resource> lut3dUpload;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT lut3dLayout = {};
    {
        std::vector<BYTE> table;
        RETURN_IF_FAILED_QUIET(ReadWholeFile(options.lut3dPath, table));

        const size_t entryCount = static_cast<size_t>(options.lut3dSize) * options.lut3dSize *
                                  options.lut3dSize;
        const size_t expectedBytes = entryCount * kLutEntryBytes;
        if (table.size() != expectedBytes)
        {
            std::wcerr << L"3D LUT size mismatch: expected " << std::dec << expectedBytes
                       << L" bytes for a " << options.lut3dSize << L"^3 RGBA16 table, got "
                       << table.size() << L".\n";
            return E_INVALIDARG;
        }

        const D3D12_RESOURCE_DESC lut3dDesc = resources.lut3d->GetDesc();
        UINT64 stagingBytes = 0;
        device->GetCopyableFootprints(&lut3dDesc, 0, 1, 0, &lut3dLayout, nullptr, nullptr,
                                      &stagingBytes);

        const D3D12_RESOURCE_DESC stagingDesc = MakeBufferDesc(stagingBytes);
        RETURN_IF_FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
                                                         &stagingDesc,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&lut3dUpload)));

        MappedResource mapped;
        RETURN_IF_FAILED_QUIET(mapped.Map(lut3dUpload.Get()));

        // The file is tightly packed; the staging buffer has a row pitch per depth slice.
        const UINT64 sourceRowPitch = static_cast<UINT64>(options.lut3dSize) * kLutEntryBytes;
        const UINT64 destinationRowPitch = lut3dLayout.Footprint.RowPitch;

        for (UINT slice = 0; slice < lut3dLayout.Footprint.Depth; ++slice)
        {
            CopyRows(mapped.Data() + lut3dLayout.Offset +
                         slice * lut3dLayout.Footprint.Height * destinationRowPitch,
                     destinationRowPitch,
                     table.data() + slice * lut3dLayout.Footprint.Height * sourceRowPitch,
                     sourceRowPitch, lut3dLayout.Footprint.Height);
        }
    }

    RETURN_IF_FAILED(context.copyAllocator->Reset());
    RETURN_IF_FAILED(context.copyList->Reset(context.copyAllocator.Get(), nullptr));

    const D3D12_TEXTURE_COPY_LOCATION lut1dSource =
        MakeFootprintLocation(lut1dUpload.Get(), lut1dLayout);
    const D3D12_TEXTURE_COPY_LOCATION lut1dDestination =
        MakeSubresourceLocation(resources.lut1d.Get(), 0);
    context.copyList->CopyTextureRegion(&lut1dDestination, 0, 0, 0, &lut1dSource, nullptr);

    const D3D12_TEXTURE_COPY_LOCATION lut3dSource =
        MakeFootprintLocation(lut3dUpload.Get(), lut3dLayout);
    const D3D12_TEXTURE_COPY_LOCATION lut3dDestination =
        MakeSubresourceLocation(resources.lut3d.Get(), 0);
    context.copyList->CopyTextureRegion(&lut3dDestination, 0, 0, 0, &lut3dSource, nullptr);

    RETURN_IF_FAILED(context.copyList->Close());
    RETURN_IF_FAILED_QUIET(context.ExecuteAndWait(context.copyQueue.Get(), context.copyList.Get()));

    std::wcout << L"Uploaded the 1D and 3D LUT tables.\n";
    return S_OK;
}

// Both tables are written once and then read by every ProcessFrames2 call, so they are
// moved into VIDEO_PROCESS_READ once here and left there for the rest of the run.
HRESULT TransitionLutTablesForRead(CommandContext& context, const Resources& resources)
{
    RETURN_IF_FAILED(context.videoAllocator->Reset());
    RETURN_IF_FAILED(context.videoList->Reset(context.videoAllocator.Get()));

    const D3D12_RESOURCE_BARRIER barriers[] = {
        MakeTransitionBarrier(resources.lut1d.Get(), D3D12_RESOURCE_STATE_COMMON,
                              D3D12_RESOURCE_STATE_VIDEO_PROCESS_READ),
        MakeTransitionBarrier(resources.lut3d.Get(), D3D12_RESOURCE_STATE_COMMON,
                              D3D12_RESOURCE_STATE_VIDEO_PROCESS_READ),
    };
    context.videoList->ResourceBarrier(CountOf(barriers), barriers);

    RETURN_IF_FAILED(context.videoList->Close());
    RETURN_IF_FAILED_QUIET(context.ExecuteAndWait(context.videoQueue.Get(), context.videoList.Get()));
    return S_OK;
}

//---------------------------------------------------------------------------------------
// 9. Per-frame work
//---------------------------------------------------------------------------------------

HRESULT UploadFrame(CommandContext& context, const Resources& resources,
                    const std::vector<BYTE>& frame)
{
    {
        MappedResource mapped;
        RETURN_IF_FAILED_QUIET(mapped.Map(resources.inputUpload.Get()));

        // P010: a 16-bit luma plane at full resolution, then a 16-bit interleaved chroma
        // plane at half height with the same row pitch.
        const UINT64 sourceRowPitch = static_cast<UINT64>(kInputStreamWidth) * 2;
        const UINT64 lumaBytesInFile = sourceRowPitch * kInputStreamHeight;

        CopyRows(mapped.Data() + resources.inputLayout[0].Offset,
                 resources.inputLayout[0].Footprint.RowPitch, frame.data(), sourceRowPitch,
                 resources.inputRowCount[0]);

        CopyRows(mapped.Data() + resources.inputLayout[1].Offset,
                 resources.inputLayout[1].Footprint.RowPitch, frame.data() + lumaBytesInFile,
                 sourceRowPitch, resources.inputRowCount[1]);
    }

    RETURN_IF_FAILED(context.copyAllocator->Reset());
    RETURN_IF_FAILED(context.copyList->Reset(context.copyAllocator.Get(), nullptr));

    for (UINT plane = 0; plane < CountOf(resources.inputLayout); ++plane)
    {
        const D3D12_TEXTURE_COPY_LOCATION source =
            MakeFootprintLocation(resources.inputUpload.Get(), resources.inputLayout[plane]);
        const D3D12_TEXTURE_COPY_LOCATION destination =
            MakeSubresourceLocation(resources.input.Get(), plane);
        context.copyList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }

    RETURN_IF_FAILED(context.copyList->Close());
    RETURN_IF_FAILED_QUIET(context.ExecuteAndWait(context.copyQueue.Get(), context.copyList.Get()));
    return S_OK;
}

HRESULT ProcessFrame(CommandContext& context,
                     const Resources& resources,
                     ID3D12VideoProcessor2* processor,
                     const D3D12_VIDEO_PROCESS_LUT_TRANSFORM_CONFIGURATION& lutConfiguration,
                     bool uploadLutState)
{
    RETURN_IF_FAILED(context.videoAllocator->Reset());
    RETURN_IF_FAILED(context.videoList->Reset(context.videoAllocator.Get()));

    D3D12_VIDEO_PROCESS_TRANSFORM_LUT transform = {};
    transform.Configuration = lutConfiguration;

    // The CSC matrix and both tables are latched into the processor when the matching
    // update flag is set, and reused by every later frame. Only the first frame pays for
    // it; after that the flags stay clear.
    transform.UpdateFlags = uploadLutState ? (D3D12_VIDEO_PROCESS_LUT_UPDATE_FLAG_CSC |
                                              D3D12_VIDEO_PROCESS_LUT_UPDATE_FLAG_1DLUT |
                                              D3D12_VIDEO_PROCESS_LUT_UPDATE_FLAG_3DLUT)
                                           : D3D12_VIDEO_PROCESS_LUT_UPDATE_FLAG_NONE;

    std::memcpy(transform.pCSCMatrix, kBt2020YuvToRgbMatrix, sizeof(kBt2020YuvToRgbMatrix));
    transform.p1DLUTtransform = resources.lut1d.Get();
    transform.p3DLUTTransform = resources.lut3d.Get();

    D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS2 inputArguments = {};
    inputArguments.InputStream[0].pTexture2D = resources.input.Get();
    inputArguments.Transform.SourceRectangle = { 0, 0, static_cast<LONG>(kInputStreamWidth),
                                                 static_cast<LONG>(kInputStreamHeight) };
    inputArguments.Transform.DestinationRectangle = { 0, 0, static_cast<LONG>(kOutputStreamWidth),
                                                      static_cast<LONG>(kOutputStreamHeight) };
    inputArguments.LUTMode = transform;

    D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS outputArguments = {};
    outputArguments.OutputStream[0].pTexture2D = resources.output.Get();
    outputArguments.TargetRectangle = { 0, 0, static_cast<LONG>(kOutputStreamWidth),
                                        static_cast<LONG>(kOutputStreamHeight) };

    const D3D12_RESOURCE_BARRIER toVideoProcess[] = {
        MakeTransitionBarrier(resources.input.Get(), D3D12_RESOURCE_STATE_COMMON,
                              D3D12_RESOURCE_STATE_VIDEO_PROCESS_READ),
        MakeTransitionBarrier(resources.output.Get(), D3D12_RESOURCE_STATE_COMMON,
                              D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE),
    };
    context.videoList->ResourceBarrier(CountOf(toVideoProcess), toVideoProcess);

    context.videoListLut->ProcessFrames2(processor, &outputArguments, 1, &inputArguments);

    // Back to COMMON so the copy queue can read the output and the next iteration starts
    // from the same state it assumed above.
    const D3D12_RESOURCE_BARRIER toCommon[] = {
        MakeTransitionBarrier(resources.input.Get(), D3D12_RESOURCE_STATE_VIDEO_PROCESS_READ,
                              D3D12_RESOURCE_STATE_COMMON),
        MakeTransitionBarrier(resources.output.Get(), D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE,
                              D3D12_RESOURCE_STATE_COMMON),
    };
    context.videoList->ResourceBarrier(CountOf(toCommon), toCommon);

    RETURN_IF_FAILED(context.videoList->Close());
    RETURN_IF_FAILED_QUIET(context.ExecuteAndWait(context.videoQueue.Get(), context.videoList.Get()));
    return S_OK;
}

HRESULT ReadbackFrame(CommandContext& context, const Resources& resources,
                      std::vector<BYTE>& frame)
{
    RETURN_IF_FAILED(context.copyAllocator->Reset());
    RETURN_IF_FAILED(context.copyList->Reset(context.copyAllocator.Get(), nullptr));

    for (UINT plane = 0; plane < CountOf(resources.outputLayout); ++plane)
    {
        const D3D12_TEXTURE_COPY_LOCATION source =
            MakeSubresourceLocation(resources.output.Get(), plane);
        const D3D12_TEXTURE_COPY_LOCATION destination =
            MakeFootprintLocation(resources.outputReadback.Get(), resources.outputLayout[plane]);
        context.copyList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }

    // No barriers: output is promoted to COPY_SOURCE and decays back to COMMON when the
    // copy queue finishes.
    RETURN_IF_FAILED(context.copyList->Close());
    RETURN_IF_FAILED_QUIET(context.ExecuteAndWait(context.copyQueue.Get(), context.copyList.Get()));

    // Drop the row padding to get a tightly packed NV12 frame.
    frame.resize(static_cast<size_t>(kOutputFrameBytes));

    MappedResource mapped;
    RETURN_IF_FAILED_QUIET(mapped.Map(resources.outputReadback.Get()));

    const UINT64 lumaBytes = static_cast<UINT64>(kOutputStreamWidth) * kOutputStreamHeight;

    CopyRows(frame.data(), kOutputStreamWidth, mapped.Data() + resources.outputLayout[0].Offset,
             resources.outputLayout[0].Footprint.RowPitch, kOutputStreamHeight);

    CopyRows(frame.data() + lumaBytes, kOutputStreamWidth,
             mapped.Data() + resources.outputLayout[1].Offset,
             resources.outputLayout[1].Footprint.RowPitch, kOutputStreamHeight / 2);

    return S_OK;
}

//---------------------------------------------------------------------------------------
// 10. Run
//---------------------------------------------------------------------------------------

HRESULT Run(const Options& options)
{
    Lut3DParameters lut3d = {};
    RETURN_IF_FAILED_QUIET(Resolve3DLutParameters(options.lut3dSize, lut3d));

    FrameReader reader;
    RETURN_IF_FAILED_QUIET(reader.Open(options.inputPath, kInputFrameBytes));
    std::wcout << L"Input holds " << std::dec << reader.FrameCount() << L" frame(s).\n";

    if (options.enableDebugLayer)
    {
        EnableDebugLayer();
    }

    ComPtr<ID3D12Device> device;
    RETURN_IF_FAILED_QUIET(CreateDevice(options, device));
    g_deviceForDiagnostics = device.Get();

    CommandContext context;
    RETURN_IF_FAILED_QUIET(CreateCommandContext(device, context));
    RETURN_IF_FAILED_QUIET(CheckVideoProcessSupport(context.videoDevice.Get(), options, lut3d));

    const D3D12_VIDEO_PROCESS_LUT_TRANSFORM_CONFIGURATION lutConfiguration =
        MakeLutConfiguration(lut3d);
    const D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC1 inputStreamDesc =
        MakeInputStreamDesc(lutConfiguration);
    const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC outputStreamDesc = MakeOutputStreamDesc();

    ComPtr<ID3D12VideoProcessor2> processor;
    RETURN_IF_FAILED_QUIET(CreateVideoProcessor(context.videoDevice.Get(), inputStreamDesc,
                                                outputStreamDesc, processor));

    Resources resources;
    RETURN_IF_FAILED_QUIET(CreateStreamResources(device.Get(), resources));
    RETURN_IF_FAILED_QUIET(CreateLutResources(device.Get(), options, resources));
    RETURN_IF_FAILED_QUIET(UploadLutTables(context, options, resources));
    RETURN_IF_FAILED_QUIET(TransitionLutTablesForRead(context, resources));

    std::vector<BYTE> inputFrame;
    std::vector<BYTE> outputFrame;

    for (UINT frameIndex = 0; frameIndex < reader.FrameCount(); ++frameIndex)
    {
        std::wcout << L"\n--- Frame " << std::dec << frameIndex << L" ---\n";

        RETURN_IF_FAILED_QUIET(reader.ReadFrame(frameIndex, inputFrame));
        RETURN_IF_FAILED_QUIET(UploadFrame(context, resources, inputFrame));
        RETURN_IF_FAILED_QUIET(ProcessFrame(context, resources, processor.Get(), lutConfiguration,
                                            frameIndex == 0));
        RETURN_IF_FAILED_QUIET(ReadbackFrame(context, resources, outputFrame));

        const std::wstring outputPath = MakeOutputPath(options.outputPathBase, frameIndex);
        RETURN_IF_FAILED_QUIET(WriteWholeFile(outputPath, outputFrame));
        std::wcout << L"Wrote " << outputPath << L"\n";
    }

    std::wcout << L"\nProcessed " << std::dec << reader.FrameCount() << L" frame(s).\n";
    return S_OK;
}

} // namespace

int main()
{
    const CommandLine commandLine;

    Options options;
#if defined(D3D12VPBLITLUT_ENABLE_DEBUG_LAYER)
    options.enableDebugLayer = true;
#endif

    if (FAILED(ParseCommandLine(commandLine, options)))
    {
        PrintUsage();
        return EXIT_FAILURE;
    }

    const HRESULT hr = Run(options);

    // The diagnostics pointer must not outlive the device Run() owns.
    g_deviceForDiagnostics = nullptr;

    return SUCCEEDED(hr) ? EXIT_SUCCESS : EXIT_FAILURE;
}
