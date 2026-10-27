// ArchiveCommon.h : internal helpers shared by the ZIP and TAR writers (not part of the public API).
#pragma once

#include "ZipEngine.h"

#include <string>
#include <vector>

namespace ZipEngine
{
    namespace detail
    {
        // UTF-8 <-> UTF-16 for Win32 file APIs. ToWide returns false for invalid UTF-8.
        bool ToWide(const std::string& utf8, std::wstring& wide);
        std::string ToUtf8(const std::wstring& wide);

        // Win32 error code -> UTF-8 text without the trailing period/newline.
        std::string Win32Message(unsigned long error);

        // The part after the last '\' or '/', i.e. the name the entry gets inside the archive.
        std::string BaseName(const std::string& path);

        // Result of ValidateInputs: the entry name and size of every input, in order.
        struct ValidatedInputs
        {
            std::vector<std::string>        entryNames;   // UTF-8, unique (case-insensitively)
            std::vector<unsigned long long> sizes;        // bytes
            unsigned long long              totalBytes = 0;
        };

        // Phase 1 of every writer: checks the arguments so common mistakes are reported with the
        // offending file name before anything is written:
        //  - output path and file list are not empty, all paths are valid UTF-8
        //  - the output folder exists, and the output is not also one of the inputs
        //  - every input exists, is a file (not a folder) and can be opened for reading
        //  - no two inputs share a file name (entries are stored without folders)
        // Returns Status::Success, or the failure status with a message in `error`.
        Status ValidateInputs(const std::string& outputPath, const std::vector<std::string>& files,
            ValidatedInputs& result, std::string& error);

    } // namespace detail
} // namespace ZipEngine
