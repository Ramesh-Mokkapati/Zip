// TarEngine.cpp
// TAR creation with libarchive, optionally compressed with gzip, bzip2 or xz.
//
// Unlike libzip, libarchive streams: it writes the archive as entries are added, so this
// writer produces a temporary file next to the output and renames it over the output only
// after archive_write_close() succeeds. Any failure or cancellation deletes the temporary
// file, which gives the same guarantee as the ZIP writer: an existing archive is never
// half-overwritten.
//
// Format: "pax restricted" = classic ustar headers, plus pax extended headers only when an
// entry needs them (non-ASCII names, names longer than 100 bytes, files over 8 GB). This is
// what bsdtar uses by default and what GNU tar, bsdtar (Windows tar.exe) and 7-Zip read.

#include "ZipEngine.h"
#include "ArchiveCommon.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <archive.h>
#include <archive_entry.h>

#include <memory>

namespace ZipEngine
{
    namespace
    {
        // Seconds since 1970 for a Win32 FILETIME (100 ns ticks since 1601).
        long long ToUnixTime(const FILETIME& ft)
        {
            const unsigned long long ticks = (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
            return static_cast<long long>(ticks / 10000000ULL) - 11644473600LL;
        }

        struct ArchiveWriteDeleter
        {
            void operator()(archive* a) const
            {
                archive_write_free(a);
            }
        };

        struct EntryDeleter
        {
            void operator()(archive_entry* e) const
            {
                archive_entry_free(e);
            }
        };

        struct HandleCloser
        {
            void operator()(HANDLE h) const
            {
                if (h && h != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(h);
                }
            }
        };

        typedef std::unique_ptr<archive, ArchiveWriteDeleter> ArchivePtr;
        typedef std::unique_ptr<archive_entry, EntryDeleter>  EntryPtr;
        typedef std::unique_ptr<void, HandleCloser>           HandlePtr;

        // Deletes the temporary file unless Keep() was called (i.e. on every failure path).
        class TempFileGuard
        {
        public:
            explicit TempFileGuard(const std::wstring& path) : m_path(path) {}

            ~TempFileGuard()
            {
                if (!m_kept)
                {
                    DeleteFileW(m_path.c_str());
                }
            }

            void Keep()
            {
                m_kept = true;
            }

        private:
            std::wstring m_path;
            bool m_kept = false;
        };

    } // namespace

    Status TarCompressor::Fail(Status status, const std::string& message)
    {
        m_lastError = message;
        return status;
    }

    Status TarCompressor::CreateTar(
        const std::string& outputTarPath,
        const std::vector<std::string>& filesToAdd,
        ArchiveFormat format,
        ProgressCallback progress)
    {
        m_lastError.clear();

        if (format == ArchiveFormat::Zip)
        {
            return Fail(Status::InvalidArgument, "TarCompressor writes TAR formats only; use ZipCompressor for ZIP.");
        }

        // ---- Phase 1: validate (shared rules with the ZIP writer) ------------------------
        detail::ValidatedInputs inputs;
        std::string error;
        const Status valid = detail::ValidateInputs(outputTarPath, filesToAdd, inputs, error);
        if (valid != Status::Success)
        {
            return Fail(valid, error);
        }

        std::wstring outputWide;
        detail::ToWide(outputTarPath, outputWide);

        // ---- Phase 2: open a temporary file in the output folder ----------------------------
        // Same folder => same volume, so the final rename is a cheap, atomic replace.
        wchar_t fullOut[MAX_PATH * 4] = {};
        GetFullPathNameW(outputWide.c_str(), static_cast<DWORD>(_countof(fullOut)), fullOut, nullptr);
        std::wstring dir(fullOut);
        dir = dir.substr(0, dir.find_last_of(L"\\/"));
        wchar_t tempPath[MAX_PATH * 4] = {};

        if (!GetTempFileNameW(dir.c_str(), L"tar", 0, tempPath))
        {
            return Fail(Status::ArchiveCreateError, "Cannot create a temporary file in " + detail::ToUtf8(dir) + ": " +
                detail::Win32Message(GetLastError()));
        }

        TempFileGuard tempGuard(tempPath);

        ArchivePtr a(archive_write_new());
        if (!a)
        {
            return Fail(Status::UnknownError, "libarchive could not allocate a writer.");
        }

        int rc = archive_write_set_format_pax_restricted(a.get());
        switch (format)
        {

        case ArchiveFormat::TarGz:
            if (rc == ARCHIVE_OK)
            {
                rc = archive_write_add_filter_gzip(a.get());
            }
            break;

        case ArchiveFormat::TarBz2:
            if (rc == ARCHIVE_OK)
            {
                rc = archive_write_add_filter_bzip2(a.get());
            }
            break;

        case ArchiveFormat::TarXz:
            if (rc == ARCHIVE_OK)
            {
                rc = archive_write_add_filter_xz(a.get());
            }
            break;

        default:
            break;   // plain .tar
        }

        if (rc != ARCHIVE_OK)
        {
            return Fail(Status::UnknownError, std::string("Cannot set up the ") + FormatName(format) + " writer: " +
                (archive_error_string(a.get()) ? archive_error_string(a.get()) : "?"));
        }

        // Wide-character open, so any output path works.
        if (archive_write_open_filename_w(a.get(), tempPath) != ARCHIVE_OK)
        {
            return Fail(Status::ArchiveCreateError, "Cannot write " + outputTarPath + ": " +
                (archive_error_string(a.get()) ? archive_error_string(a.get()) : "?"));
        }

        // ---- Phase 3: add each file, streaming its data -------------------------------------
        std::vector<char> buffer(256 * 1024);
        unsigned long long done = 0;
        if (progress && !progress(0.0))
        {
            return Fail(Status::Cancelled, "Cancelled; no archive was written.");
        }

        for (std::size_t i = 0; i < filesToAdd.size(); ++i)
        {
            std::wstring wide;
            detail::ToWide(filesToAdd[i], wide);

            HandlePtr file(CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
            if (file.get() == INVALID_HANDLE_VALUE)
            {
                return Fail(Status::FileReadError, "Cannot read " + filesToAdd[i] + ": " + detail::Win32Message(GetLastError()));
            }

            BY_HANDLE_FILE_INFORMATION info = {};
            GetFileInformationByHandle(file.get(), &info);
            const unsigned long long size = (static_cast<unsigned long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;

            // The header: name (as UTF-16, so libarchive can emit a UTF-8 pax path), regular
            // file, rw-r--r--, size and modification time.
            std::wstring nameWide;
            detail::ToWide(inputs.entryNames[i], nameWide);
            EntryPtr entry(archive_entry_new());
            archive_entry_copy_pathname_w(entry.get(), nameWide.c_str());
            archive_entry_set_filetype(entry.get(), AE_IFREG);
            archive_entry_set_perm(entry.get(), 0644);
            archive_entry_set_size(entry.get(), static_cast<la_int64_t>(size));
            archive_entry_set_mtime(entry.get(), static_cast<time_t>(ToUnixTime(info.ftLastWriteTime)), 0);

            if (archive_write_header(a.get(), entry.get()) != ARCHIVE_OK)
            {
                return Fail(Status::UnknownError, "Cannot add " + filesToAdd[i] + ": " +
                    (archive_error_string(a.get()) ? archive_error_string(a.get()) : "?"));
            }

            // Copy exactly `size` bytes (the size promised in the header), even if the file
            // changes while it is being read.
            unsigned long long remaining = size;
            while (remaining > 0)
            {
                const DWORD want = static_cast<DWORD>(remaining < buffer.size() ? remaining : buffer.size());
                DWORD got = 0;
                if (!ReadFile(file.get(), buffer.data(), want, &got, nullptr))
                {
                    return Fail(Status::FileReadError, "Cannot read " + filesToAdd[i] + ": " + detail::Win32Message(GetLastError()));
                }

                if (got == 0)
                {
                    return Fail(Status::FileReadError, filesToAdd[i] + " became shorter while it was being archived.");
                }

                if (archive_write_data(a.get(), buffer.data(), got) < 0)
                {
                    return Fail(Status::ArchiveCreateError, "Cannot write " + outputTarPath + ": " +
                        (archive_error_string(a.get()) ? archive_error_string(a.get()) : "?"));
                }

                remaining -= got;
                done += got;

                if (progress && inputs.totalBytes > 0 &&
                    !progress(static_cast<double>(done) / static_cast<double>(inputs.totalBytes)))
                {
                    return Fail(Status::Cancelled, "Cancelled; no archive was written.");
                }
            }
        }

        // Close flushes the compressor and writes the tar end-of-archive blocks.
        if (archive_write_close(a.get()) != ARCHIVE_OK)
        {
            return Fail(Status::ArchiveCreateError, "Cannot finish " + outputTarPath + ": " +
                (archive_error_string(a.get()) ? archive_error_string(a.get()) : "?"));
        }

        a.reset();

        // ---- Phase 4: replace the output in one step -----------------------------------------
        if (!MoveFileExW(tempPath, outputWide.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            return Fail(Status::ArchiveCreateError, "Cannot write " + outputTarPath + ": " + detail::Win32Message(GetLastError()));
        }

        tempGuard.Keep();

        if (progress)
        {
            progress(1.0);
        }

        return Status::Success;
    }

} // namespace ZipEngine
