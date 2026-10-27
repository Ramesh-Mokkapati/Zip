// ArchiveCommon.cpp : input validation and string helpers shared by the ZIP and TAR writers.

#include "ArchiveCommon.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cwctype>

namespace ZipEngine
{
    namespace detail
    {
        namespace
        {
            // Absolute, normalised path (resolves "..", "." and relative paths) for comparisons.
            std::wstring FullPath(const std::wstring& path)
            {
                const DWORD n = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
                if (n == 0)
                {
                    return path;
                }

                std::wstring full(n, L'\0');
                const DWORD written = GetFullPathNameW(path.c_str(), n, &full[0], nullptr);
                full.resize(written);
                return full;
            }

            std::wstring Lower(std::wstring s)
            {
                std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
                return s;
            }

        } // namespace

        bool ToWide(const std::string& utf8, std::wstring& wide)
        {
            wide.clear();
            if (utf8.empty())
            {
                return true;
            }

            const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
            if (n <= 0)
            {
                return false;
            }

            wide.resize(static_cast<size_t>(n));
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), &wide[0], n);
            return true;
        }

        std::string ToUtf8(const std::wstring& wide)
        {
            if (wide.empty())
            {
                return std::string();
            }

            const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
            std::string out(static_cast<size_t>(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), &out[0], n, nullptr, nullptr);
            return out;
        }

        std::string Win32Message(unsigned long error)
        {
            wchar_t* buf = nullptr;
            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr, error, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
            std::wstring text = buf ? buf : L"Unknown error";
            if (buf)
            {
                LocalFree(buf);
            }

            while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L'.'))
            {
                text.pop_back();
            }

            return ToUtf8(text);
        }

        std::string BaseName(const std::string& path)
        {
            const std::size_t found = path.find_last_of("/\\");
            return (found != std::string::npos) ? path.substr(found + 1) : path;
        }

        Status ValidateInputs(const std::string& outputPath, const std::vector<std::string>& files,
            ValidatedInputs& result, std::string& error)
        {
            result = ValidatedInputs();

            if (outputPath.empty())
            {
                error = "No output file was given.";
                return Status::InvalidArgument;
            }

            if (files.empty())
            {
                error = "No files were given to add to the archive.";
                return Status::InvalidArgument;
            }

            std::wstring outputWide;
            if (!ToWide(outputPath, outputWide))
            {
                error = "The output path is not valid UTF-8.";
                return Status::InvalidArgument;
            }

            const std::wstring outputFull = FullPath(outputWide);
            const std::wstring outputKey = Lower(outputFull);

            // The folder must exist: the writers would otherwise only discover this at the very end.
            const std::wstring outputDir = outputFull.substr(0, outputFull.find_last_of(L"\\/"));
            const DWORD dirAttrs = GetFileAttributesW(outputDir.c_str());
            if (dirAttrs == INVALID_FILE_ATTRIBUTES || !(dirAttrs & FILE_ATTRIBUTE_DIRECTORY))
            {
                error = "The output folder does not exist: " + ToUtf8(outputDir);
                return Status::ArchiveCreateError;
            }

            std::vector<std::wstring> seenNames;   // lower-case, for the duplicate check
            for (const auto& filePath : files)
            {
                std::wstring wide;
                if (!ToWide(filePath, wide) || wide.empty())
                {
                    error = "An input path is empty or not valid UTF-8.";
                    return Status::InvalidArgument;
                }

                WIN32_FILE_ATTRIBUTE_DATA info = {};
                if (!GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &info))
                {
                    error = "File not found: " + filePath;
                    return Status::FileNotFound;
                }

                if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    error = "This is a folder, not a file: " + filePath;
                    return Status::FileNotFound;
                }

                // Archiving the output into itself would read a file that is being replaced.
                if (Lower(FullPath(wide)) == outputKey)
                {
                    error = "The output archive cannot also be one of the input files: " + filePath;
                    return Status::InvalidArgument;
                }

                // Open it now so a locked or protected file is reported by name, instead of as an
                // anonymous read error halfway through compression.
                HANDLE h = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h == INVALID_HANDLE_VALUE)
                {
                    error = "Cannot read " + filePath + ": " + Win32Message(GetLastError());
                    return Status::FileReadError;
                }

                CloseHandle(h);

                // Entries are stored by file name only, so two inputs with the same name (from
                // different folders) would collide. Windows file names are case-insensitive, so
                // "a.txt" and "A.TXT" would also collide when the archive is extracted.
                const std::string name = BaseName(filePath);
                std::wstring nameWide;
                ToWide(name, nameWide);

                const std::wstring key = Lower(nameWide);
                if (std::find(seenNames.begin(), seenNames.end(), key) != seenNames.end())
                {
                    error = "More than one file is named \"" + name + "\". Rename one of them or add them separately.";
                    return Status::DuplicateEntryName;
                }

                seenNames.push_back(key);

                const unsigned long long size = (static_cast<unsigned long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
                result.entryNames.push_back(name);
                result.sizes.push_back(size);
                result.totalBytes += size;
            }

            return Status::Success;
        }

    } // namespace detail
} // namespace ZipEngine
