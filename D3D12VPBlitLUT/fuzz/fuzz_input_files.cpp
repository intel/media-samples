/*
 * Copyright (C) 2026 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */


//=======================================================================================
// libFuzzer target for the file readers: ReadWholeFile(), which loads the LUT tables,
// and FrameReader, which reads the input video.
//
// The sample never interprets file contents on the CPU - frames and LUT entries go to
// the GPU as they are - so the size checks and read paths are what can go wrong here.
// The real frame and LUT sizes run to megabytes, out of a fuzzer's reach, so the
// expected sizes come from the input instead:
//
//   byte 0     FrameReader frame size minus one (1..256 bytes)
//   byte 1     ReadWholeFile expected size: the file size if the top bit is set,
//              otherwise the low 7 bits, so that mismatches are tried too
//   bytes 2..  the file contents
//
// Each input is written to a per-process file in %TEMP%, deleted when the fuzzer exits.
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

const std::wstring& ScratchPath()
{
    static const struct ScratchFile
    {
        ScratchFile()
        {
            wchar_t directory[MAX_PATH + 1] = {};
            ::GetTempPathW(CountOf(directory), directory);
            path = std::wstring(directory) + L"D3D12VPBlitLUT_fuzz_" +
                   std::to_wstring(::GetCurrentProcessId()) + L".bin";
        }

        ~ScratchFile() { ::DeleteFileW(path.c_str()); }

        std::wstring path;
    } scratch;

    return scratch.path;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 2)
    {
        return 0;
    }

    const UINT64 frameBytes = static_cast<UINT64>(data[0]) + 1;
    const std::vector<BYTE> payload(data + 2, data + size);
    const UINT64 expectedBytes = (data[1] & 0x80) ? payload.size() : (data[1] & 0x7F);

    const std::wstring& path = ScratchPath();
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(payload.data()),
                   static_cast<std::streamsize>(payload.size()));
        Check(file.good(), "the harness can write its scratch file");
    }

    std::vector<BYTE> contents;
    if (SUCCEEDED(ReadWholeFile(path, expectedBytes, contents)))
    {
        Check(contents == payload, "ReadWholeFile returns the file contents");
    }
    else
    {
        Check(expectedBytes != payload.size(), "ReadWholeFile accepts a file of the expected size");
    }

    FrameReader reader;
    if (SUCCEEDED(reader.Open(path, frameBytes)))
    {
        Check(reader.FrameCount() == payload.size() / frameBytes,
              "FrameReader counts only whole frames");

        std::vector<BYTE> frame;
        for (UINT index = 0; index < reader.FrameCount(); ++index)
        {
            Check(SUCCEEDED(reader.ReadFrame(index, frame)), "FrameReader reads every whole frame");

            const auto frameStart = payload.begin() + static_cast<std::ptrdiff_t>(index * frameBytes);
            Check(frame.size() == frameBytes && std::equal(frame.begin(), frame.end(), frameStart),
                  "FrameReader returns the frame's bytes");
        }
    }
    else
    {
        Check(payload.size() < frameBytes, "FrameReader accepts a file holding a whole frame");
    }

    return 0;
}
