/*
 * Copyright (C) 2026 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */


//=======================================================================================
// libFuzzer target for the command line: ParseCommandLine(), and for every command line
// it accepts, the code that consumes the parsed values (Resolve3DLutParameters() and
// MakeOutputPath()).
//
// The input is the argument list in UTF-8, one argument per line. Each argument is
// converted to UTF-16, which is how the sample receives it from CommandLineToArgvW;
// invalid UTF-8 becomes U+FFFD. Seeds are in corpus/command_line/, and
// command_line.dict lists the option names.
//=======================================================================================

#define D3D12VPBLITLUT_FUZZING
#include "../D3D12VPBlitLUT.cpp"

#include <cstdint>
#include <cstdio>

namespace {

// Logic errors are turned into crashes so that libFuzzer saves the input.
void Check(bool condition, const char* invariant)
{
    if (!condition)
    {
        std::fprintf(stderr, "Invariant violated: %s\n", invariant);
        std::abort();
    }
}

std::wstring Utf8ToUtf16(const char* text, int length)
{
    if (length == 0)
    {
        return std::wstring();
    }

    const int wideLength = ::MultiByteToWideChar(CP_UTF8, 0, text, length, nullptr, 0);
    std::wstring wide(static_cast<size_t>(wideLength), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text, length, &wide[0], wideLength);
    return wide;
}

void CheckOutputPaths(const std::wstring& base)
{
    // Stripping the extension must never reach into the directory part.
    const size_t separator = base.find_last_of(L"\\/");
    const size_t nameStart = (separator == std::wstring::npos) ? 0 : separator + 1;

    const UINT frameIndices[] = { 0, 7, 999, 1000, UINT_MAX };
    for (UINT frameIndex : frameIndices)
    {
        const std::wstring path = MakeOutputPath(base, frameIndex);

        Check(path.compare(0, nameStart, base, 0, nameStart) == 0,
              "MakeOutputPath keeps the directory part of the base");
        Check(path.find_first_of(L"\\/", nameStart) == std::wstring::npos,
              "MakeOutputPath adds no path separators");
        Check(path.size() >= 4 && path.compare(path.size() - 4, 4, L".bin") == 0,
              "MakeOutputPath ends in .bin");
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    // Keeps every argument length within an int; libFuzzer's -max_len is far below this.
    if (size > (1u << 20))
    {
        return 0;
    }

    std::vector<std::wstring> argumentText;
    argumentText.emplace_back(L"D3D12VPBlitLUT.exe");

    const char* text = reinterpret_cast<const char*>(data);
    size_t start = 0;
    for (size_t i = 0; i < size; ++i)
    {
        if (text[i] == '\n')
        {
            argumentText.push_back(Utf8ToUtf16(text + start, static_cast<int>(i - start)));
            start = i + 1;
        }
    }
    if (start < size)
    {
        argumentText.push_back(Utf8ToUtf16(text + start, static_cast<int>(size - start)));
    }

    std::vector<const wchar_t*> arguments;
    for (const std::wstring& argument : argumentText)
    {
        arguments.push_back(argument.c_str());
    }

    Options options;
    if (FAILED(ParseCommandLine(static_cast<int>(arguments.size()), arguments.data(), options)))
    {
        return 0;
    }

    Check(!options.inputPath.empty() && !options.lut1dPath.empty() && !options.lut3dPath.empty(),
          "an accepted command line has all required paths");
    Check(options.lut1dSize != 0 && options.lut3dSize != 0,
          "an accepted command line has both LUT sizes");
    Check(!options.outputPathBase.empty(), "an accepted command line has an output base");

    Lut3DParameters lut3d = {};
    if (SUCCEEDED(Resolve3DLutParameters(options.lut3dSize, lut3d)))
    {
        Check(options.lut3dSize == 17 || options.lut3dSize == 33 || options.lut3dSize == 45 ||
                  options.lut3dSize == 65,
              "Resolve3DLutParameters only accepts 17, 33, 45 and 65");
    }

    CheckOutputPaths(options.outputPathBase);
    return 0;
}
