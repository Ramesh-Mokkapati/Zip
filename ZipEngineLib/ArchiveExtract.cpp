// ArchiveExtract.cpp
// Safe extraction of ZIP (libzip: handles AES and ZipCrypto decryption) and TAR variants
// (libarchive), plus ZIP listing.
//
// Archives are treated as untrusted input. The work happens in phases, like the writers:
//   1. Read every entry header and turn each name into a safe relative path
//      (SafeRelativePath). Anything that could escape the destination ("Zip Slip"), or that
//      Windows would misinterpret (device names, ':' streams, trailing dots), stops the whole
//      extraction before a single byte is written. Links and devices are marked "skip".
//   2. Check the password (ZIP), duplicate targets, existing files, and free disk space.
//   3. Write each file to "<name>.~part" and rename it into place when complete.
// Everything created during phase 3 is recorded in a Rollback; if anything fails or the user
// cancels, those files and folders are removed again.

#include "ZipEngine.h"
#include "ArchiveCommon.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <zip.h>
#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cwctype>
#include <memory>
#include <set>

namespace ZipEngine
{
    namespace detail
    {
        bool IsZipFile(const std::wstring& path)
        {
            HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return false;
            }

            unsigned char sig[4] = { 0 };
            DWORD got = 0;
            ReadFile(h, sig, sizeof(sig), &got, nullptr);
            CloseHandle(h);
            return got == 4 && sig[0] == 'P' && sig[1] == 'K' &&
                ((sig[2] == 3 && sig[3] == 4) || (sig[2] == 5 && sig[3] == 6));
        }

        Status ListZip(const std::string& archivePath, std::vector<ArchiveEntryInfo>& entries, std::string& error)
        {
            int err = 0;
            zip_t* za = zip_open(archivePath.c_str(), ZIP_RDONLY, &err);
            if (!za)
            {
                zip_error_t ze;
                zip_error_init_with_code(&ze, err);
                error = "Cannot open " + archivePath + ": " + zip_error_strerror(&ze);
                zip_error_fini(&ze);
                return err == ZIP_ER_NOENT ? Status::FileNotFound : Status::FileReadError;
            }

            const zip_int64_t n = zip_get_num_entries(za, 0);
            for (zip_int64_t i = 0; i < n; ++i)
            {
                zip_stat_t st;
                zip_stat_init(&st);
                if (zip_stat_index(za, static_cast<zip_uint64_t>(i), 0, &st) != 0 || !st.name)
                {
                    continue;
                }

                ArchiveEntryInfo info;
                info.name = st.name;   // libzip converts legacy (CP437) names to UTF-8
                info.isDirectory = !info.name.empty() && info.name.back() == '/';
                info.size = (st.valid & ZIP_STAT_SIZE) ? st.size : 0;
                info.encrypted = (st.valid & ZIP_STAT_ENCRYPTION_METHOD) && st.encryption_method != ZIP_EM_NONE;
                entries.push_back(info);
            }

            zip_discard(za);
            return Status::Success;
        }

        namespace
        {
            // CON, PRN, AUX, NUL, COM1-9, LPT1-9 (with or without an extension) open devices
            // instead of files on Windows.
            bool IsDeviceName(const std::wstring& component)
            {
                std::wstring base = component.substr(0, component.find(L'.'));
                while (!base.empty() && base.back() == L' ')
                {
                    base.pop_back();
                }

                std::transform(base.begin(), base.end(), base.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
                if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL" || base == L"CONIN$" || base == L"CONOUT$")
                {
                    return true;
                }

                return base.size() == 4 && (base.compare(0, 3, L"COM") == 0 || base.compare(0, 3, L"LPT") == 0) &&
                    base[3] >= L'1' && base[3] <= L'9';
            }
        }

        bool SafeRelativePath(const std::string& entryName, std::wstring& relative, std::string& reason)
        {
            relative.clear();
            std::wstring name;
            if (!ToWide(entryName, name))
            {
                reason = "the name is not valid UTF-8";
                return false;
            }

            // Both separators are accepted (some Windows tools write backslashes into ZIPs).
            std::replace(name.begin(), name.end(), L'/', L'\\');
            if (name.empty())
            {
                return true;
            }

            if (name[0] == L'\\')
            {
                reason = "it is an absolute path";
                return false;
            }

            // Catches "C:\x" and "C:x" (drive letters) and "file.txt:hidden" (alternate data streams).
            if (name.find(L':') != std::wstring::npos)
            {
                reason = "it contains ':' (a drive letter or a hidden data stream)";
                return false;
            }

            size_t start = 0;
            while (start <= name.size())
            {
                const size_t end = name.find(L'\\', start);
                const std::wstring part = name.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
                start = (end == std::wstring::npos) ? name.size() + 1 : end + 1;

                if (part.empty() || part == L".")
                {
                    continue;   // "a//b" and "./a" are harmless
                }

                if (part == L"..")
                {
                    reason = "it contains '..' and would be written outside the destination folder";
                    return false;
                }

                for (wchar_t c : part)
                {
                    if (c < 0x20 || wcschr(L"<>\"|?*", c))
                    {
                        reason = "it contains a character Windows does not allow in file names";
                        return false;
                    }
                }

                // Windows silently strips trailing dots and spaces, so "a." would become "a".
                if (part.back() == L'.' || part.back() == L' ')
                {
                    reason = "a part of the name ends with a dot or a space";
                    return false;
                }

                if (IsDeviceName(part))
                {
                    reason = "it uses a Windows device name (CON, NUL, COM1, LPT1, ...)";
                    return false;
                }

                if (part.size() > 255)
                {
                    reason = "a part of the name is longer than 255 characters";
                    return false;
                }

                if (!relative.empty())
                {
                    relative += L'\\';
                }

                relative += part;
            }

            return true;
        }
    } // namespace detail

    namespace
    {
        using namespace detail;

        // \\?\ prefix so paths longer than MAX_PATH work.
        std::wstring LongPath(const std::wstring& full)
        {
            if (full.compare(0, 4, L"\\\\?\\") == 0)
            {
                return full;
            }

            if (full.compare(0, 2, L"\\\\") == 0)
            {
                return L"\\\\?\\UNC\\" + full.substr(2);
            }

            return L"\\\\?\\" + full;
        }

        std::wstring FullPath(const std::wstring& path)
        {
            const DWORD n = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
            if (n == 0)
            {
                return path;
            }

            std::wstring full(n, L'\0');
            full.resize(GetFullPathNameW(path.c_str(), n, &full[0], nullptr));
            while (full.size() > 3 && (full.back() == L'\\' || full.back() == L'/'))
            {
                full.pop_back();
            }

            return full;
        }

        bool Exists(const std::wstring& longPath)
        {
            return GetFileAttributesW(longPath.c_str()) != INVALID_FILE_ATTRIBUTES;
        }

        bool IsDirectory(const std::wstring& longPath)
        {
            const DWORD a = GetFileAttributesW(longPath.c_str());
            return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
        }

        FILETIME FromUnixTime(long long seconds)
        {
            const unsigned long long ticks = static_cast<unsigned long long>(seconds + 11644473600LL) * 10000000ULL;
            FILETIME ft;
            ft.dwLowDateTime = static_cast<DWORD>(ticks);
            ft.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
            return ft;
        }

        std::wstring Lower(std::wstring s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            return s;
        }

        // One archive entry, after phase 1.
        struct Item
        {
            std::string    name;            // as stored in the archive (UTF-8), for messages
            std::wstring   relative;        // safe relative path (empty = archive root, ignored)
            bool           isDir = false;
            bool           skip = false;    // link / device: reported, never written
            bool           encrypted = false;
            unsigned long long size = 0;
            bool           hasMtime = false;
            long long      mtime = 0;       // seconds since 1970
            zip_uint64_t   zipIndex = 0;    // ZIP entry index
        };

        // Remembers what this run created so a failure can undo it. Files that replaced an
        // existing file (overwrite = true) cannot be restored and are left in place.
        struct Rollback
        {
            std::vector<std::wstring> files;
            std::vector<std::wstring> dirs;   // in creation order
            std::wstring              temp;
            bool                      committed = false;

            ~Rollback()
            {
                if (committed)
                {
                    return;
                }

                if (!temp.empty())
                {
                    DeleteFileW(temp.c_str());
                }

                for (auto it = files.rbegin(); it != files.rend(); ++it)
                {
                    DeleteFileW(it->c_str());
                }

                for (auto it = dirs.rbegin(); it != dirs.rend(); ++it)
                {
                    RemoveDirectoryW(it->c_str());   // only succeeds if empty
                }
            }
        };

        // Creates `dirLong` and any missing parents, recording each new folder.
        bool CreateFolders(const std::wstring& dirLong, Rollback& rb, unsigned& created, std::string& error)
        {
            // Stop at a drive ("\\?\C:"), which always exists and cannot be created.
            if (IsDirectory(dirLong) || (!dirLong.empty() && dirLong.back() == L':'))
            {
                return true;
            }

            if (Exists(dirLong))
            {
                error = "A file is in the way of the folder " + ToUtf8(dirLong.substr(4));
                return false;
            }

            const size_t slash = dirLong.find_last_of(L'\\');
            if (slash != std::wstring::npos && slash > 4 && !CreateFolders(dirLong.substr(0, slash), rb, created, error))
            {
                return false;
            }

            if (!CreateDirectoryW(dirLong.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            {
                error = "Cannot create the folder " + ToUtf8(dirLong.substr(4)) + ": " + Win32Message(GetLastError());
                return false;
            }

            rb.dirs.push_back(dirLong);
            ++created;
            return true;
        }

        // Overall progress by bytes written; false means "cancel".
        struct Progress
        {
            ProgressCallback   callback;
            unsigned long long total = 0;
            unsigned long long done = 0;

            bool Step(unsigned long long bytes)
            {
                done += bytes;
                return !callback || total == 0 || callback(static_cast<double>(done) / static_cast<double>(total));
            }
        };

        // Reads the next chunk of the current entry: >0 bytes, 0 at the end, <0 on error.
        typedef std::function<long long(char* buffer, size_t capacity)> ChunkReader;

        // Phase 3 for one file: write to "<target>.~part", then rename into place.
        Status WriteEntryFile(const std::wstring& rootLong, const Item& item, const ChunkReader& read, bool overwrite,
            Rollback& rb, Progress& progress, ExtractSummary& summary, std::string& error)
        {
            const std::wstring target = rootLong + L"\\" + item.relative;
            const size_t slash = target.find_last_of(L'\\');
            unsigned created = 0;
            if (!CreateFolders(target.substr(0, slash), rb, created, error))
            {
                return Status::ArchiveCreateError;
            }

            summary.folders += created;

            const bool existed = Exists(target);
            const std::wstring temp = target + L".~part";
            HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                error = "Cannot write " + ToUtf8(target.substr(4)) + ": " + Win32Message(GetLastError());
                return Status::ArchiveCreateError;
            }

            rb.temp = temp;

            std::vector<char> buffer(256 * 1024);
            Status result = Status::Success;
            for (;;)
            {
                const long long got = read(buffer.data(), buffer.size());
                if (got == 0)
                {
                    break;
                }

                if (got < 0)
                {
                    result = Status::FileReadError;   // the caller fills in the reason
                    break;
                }

                DWORD written = 0;
                if (!::WriteFile(h, buffer.data(), static_cast<DWORD>(got), &written, nullptr) || written != got)
                {
                    error = "Cannot write " + ToUtf8(target.substr(4)) + ": " + Win32Message(GetLastError());
                    result = Status::ArchiveCreateError;
                    break;
                }

                summary.bytes += static_cast<unsigned long long>(got);
                if (!progress.Step(static_cast<unsigned long long>(got)))
                {
                    result = Status::Cancelled;
                    break;
                }
            }

            if (result == Status::Success && item.hasMtime)
            {
                const FILETIME ft = FromUnixTime(item.mtime);
                SetFileTime(h, nullptr, nullptr, &ft);
            }

            CloseHandle(h);
            if (result != Status::Success)
            {
                return result;   // Rollback deletes the temp file
            }

            const DWORD flags = MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0);
            if (!MoveFileExW(temp.c_str(), target.c_str(), flags))
            {
                const DWORD code = GetLastError();
                error = "Cannot write " + ToUtf8(target.substr(4)) + ": " + Win32Message(code);
                return code == ERROR_ALREADY_EXISTS ? Status::FileExists : Status::ArchiveCreateError;
            }

            rb.temp.clear();
            if (!existed)
            {
                rb.files.push_back(target);
            }

            ++summary.files;
            return Status::Success;
        }

        // Phase 2 checks shared by ZIP and TAR. Creates the destination folder at the end.
        Status CheckPlan(const std::vector<Item>& items, const std::wstring& rootLong, bool overwrite,
            Rollback& rb, unsigned long long& totalBytes, std::string& error)
        {
            std::set<std::wstring> files, dirs;
            unsigned existing = 0;
            std::string firstExisting;
            totalBytes = 0;

            for (const Item& item : items)
            {
                if (item.skip || item.relative.empty())
                {
                    continue;
                }

                const std::wstring key = Lower(item.relative);
                if (item.isDir)
                {
                    dirs.insert(key);
                    continue;
                }

                if (!files.insert(key).second)
                {
                    error = "The archive contains \"" + item.name + "\" more than once.";
                    return Status::UnsafeEntry;
                }

                totalBytes += item.size;
                const std::wstring target = rootLong + L"\\" + item.relative;
                if (IsDirectory(target))
                {
                    error = "A folder is in the way of " + ToUtf8(target.substr(4));
                    return Status::FileExists;
                }

                if (!overwrite && Exists(target))
                {
                    if (existing++ == 0)
                    {
                        firstExisting = ToUtf8(target.substr(4));
                    }
                }
            }

            // A file and a folder with the same path cannot both be extracted.
            for (const std::wstring& d : dirs)
            {
                if (files.count(d))
                {
                    error = "The archive contains both a file and a folder named \"" + ToUtf8(d) + "\".";
                    return Status::UnsafeEntry;
                }
            }

            if (existing > 0)
            {
                error = firstExisting + " already exists" +
                    (existing > 1 ? " (and " + std::to_string(existing - 1) + " more)" : std::string()) +
                    ". Allow replacing existing files, or choose an empty folder.";
                return Status::FileExists;
            }

            // Free space, measured on the nearest folder that already exists.
            std::wstring probe = rootLong;
            while (!IsDirectory(probe) && probe.find_last_of(L'\\') > 4)
            {
                probe = probe.substr(0, probe.find_last_of(L'\\'));
            }

            ULARGE_INTEGER avail = {};
            if (GetDiskFreeSpaceExW((probe + L"\\").c_str(), &avail, nullptr, nullptr) && avail.QuadPart < totalBytes)
            {
                error = "Not enough free disk space: the archive needs " + std::to_string(totalBytes / (1024 * 1024) + 1) +
                    " MB, " + std::to_string(avail.QuadPart / (1024 * 1024)) + " MB are free.";
                return Status::ArchiveCreateError;
            }

            unsigned created = 0;
            if (!CreateFolders(rootLong, rb, created, error))
            {
                return Status::ArchiveCreateError;
            }

            return Status::Success;
        }

        // ---- ZIP ----------------------------------------------------------------------

        struct ZipCloser { void operator()(zip_t* z) const { zip_discard(z); } };

        Status ExtractZip(const std::string& archivePath, const std::wstring& rootLong, const std::string& password,
            bool overwrite, Progress& progress, Rollback& rb, ExtractSummary& summary, std::string& error)
        {
            int err = 0;
            std::unique_ptr<zip_t, ZipCloser> za(zip_open(archivePath.c_str(), ZIP_RDONLY, &err));
            if (!za)
            {
                zip_error_t ze;
                zip_error_init_with_code(&ze, err);
                error = "Cannot open " + archivePath + ": " + zip_error_strerror(&ze);
                zip_error_fini(&ze);
                return Status::FileReadError;
            }

            // Phase 1: entries and names.
            std::vector<Item> items;
            bool anyEncrypted = false;
            const zip_int64_t n = zip_get_num_entries(za.get(), 0);
            for (zip_int64_t i = 0; i < n; ++i)
            {
                zip_stat_t st;
                zip_stat_init(&st);
                if (zip_stat_index(za.get(), static_cast<zip_uint64_t>(i), 0, &st) != 0 || !st.name)
                {
                    error = "Cannot read entry " + std::to_string(i) + " of " + archivePath + ": " + zip_strerror(za.get());
                    return Status::FileReadError;
                }

                Item item;
                item.zipIndex = static_cast<zip_uint64_t>(i);
                item.name = st.name;
                item.isDir = !item.name.empty() && (item.name.back() == '/' || item.name.back() == '\\');
                item.size = (st.valid & ZIP_STAT_SIZE) ? st.size : 0;
                item.encrypted = (st.valid & ZIP_STAT_ENCRYPTION_METHOD) && st.encryption_method != ZIP_EM_NONE;
                item.hasMtime = (st.valid & ZIP_STAT_MTIME) != 0;
                item.mtime = static_cast<long long>(st.mtime);

                // ZIPs made on Unix can contain symbolic links (file type in the high bits).
                zip_uint8_t opsys = 0;
                zip_uint32_t attributes = 0;
                if (zip_file_get_external_attributes(za.get(), item.zipIndex, 0, &opsys, &attributes) == 0 && opsys == ZIP_OPSYS_UNIX)
                {
                    const zip_uint32_t type = (attributes >> 16) & 0170000;
                    if (type != 0 && type != 0100000 && type != 040000)   // not "unset", regular file or directory
                    {
                        item.skip = true;
                    }
                }

                std::string reason;
                if (!item.skip && !SafeRelativePath(item.name, item.relative, reason))
                {
                    error = "Refusing to extract \"" + item.name + "\": " + reason + ". Nothing was extracted.";
                    return Status::UnsafeEntry;
                }

                anyEncrypted = anyEncrypted || (item.encrypted && !item.isDir && !item.skip);
                items.push_back(item);
            }

            // Phase 2a: password. Opening an encrypted entry checks the password verifier, so a
            // wrong password is reported before anything is written.
            if (anyEncrypted)
            {
                if (password.empty())
                {
                    error = "This archive is encrypted. Enter the password that was used to create it.";
                    return Status::PasswordRequired;
                }

                zip_set_default_password(za.get(), password.c_str());
                for (const Item& item : items)
                {
                    if (item.encrypted && !item.isDir && !item.skip)
                    {
                        zip_file_t* zf = zip_fopen_index(za.get(), item.zipIndex, 0);
                        if (!zf)
                        {
                            const int code = zip_error_code_zip(zip_get_error(za.get()));
                            if (code == ZIP_ER_WRONGPASSWD)
                            {
                                error = "The password is not correct for this archive.";
                                return Status::WrongPassword;
                            }

                            error = "Cannot decrypt \"" + item.name + "\": " + zip_strerror(za.get());
                            return code == ZIP_ER_ENCRNOTSUPP ? Status::EncryptionError : Status::FileReadError;
                        }

                        zip_fclose(zf);
                        break;
                    }
                }
            }

            // Phase 2b: duplicates, existing files, disk space; creates the destination.
            const Status planned = CheckPlan(items, rootLong, overwrite, rb, progress.total, error);
            if (planned != Status::Success)
            {
                return planned;
            }

            // Phase 3: write.
            for (const Item& item : items)
            {
                if (item.skip)
                {
                    summary.skipped.push_back(item.name);
                    continue;
                }

                if (item.relative.empty())
                {
                    continue;
                }

                if (item.isDir)
                {
                    unsigned created = 0;
                    if (!CreateFolders(rootLong + L"\\" + item.relative, rb, created, error))
                    {
                        return Status::ArchiveCreateError;
                    }

                    summary.folders += created;
                    continue;
                }

                zip_file_t* zf = zip_fopen_index(za.get(), item.zipIndex, 0);
                if (!zf)
                {
                    const int code = zip_error_code_zip(zip_get_error(za.get()));
                    error = "Cannot read \"" + item.name + "\": " + zip_strerror(za.get());
                    return code == ZIP_ER_WRONGPASSWD ? Status::WrongPassword : Status::FileReadError;
                }

                int readError = 0;
                const ChunkReader reader = [zf, &readError](char* buffer, size_t capacity) -> long long
                {
                    const zip_int64_t got = zip_fread(zf, buffer, capacity);
                    if (got < 0)
                    {
                        readError = zip_error_code_zip(zip_file_get_error(zf));
                    }

                    return got;
                };

                Status result = WriteEntryFile(rootLong, item, reader, true /* checked in phase 2 */, rb, progress, summary, error);
                const int closeError = zip_fclose(zf);
                if (result == Status::Success && closeError != 0)
                {
                    readError = closeError;
                    result = Status::FileReadError;
                }

                if (result == Status::FileReadError)
                {
                    // Integrity checks (CRC, or the AES authentication code) failed. With encrypted
                    // data this almost always means the password was wrong.
                    if (item.encrypted)
                    {
                        error = "\"" + item.name + "\" could not be decrypted: the password is wrong or the archive is damaged.";
                        return Status::WrongPassword;
                    }

                    zip_error_t ze;
                    zip_error_init_with_code(&ze, readError);
                    error = "\"" + item.name + "\" is damaged: " + zip_error_strerror(&ze);
                    zip_error_fini(&ze);
                    return Status::FileReadError;
                }

                if (result != Status::Success)
                {
                    return result;
                }
            }

            return Status::Success;
        }

        // ---- TAR ----------------------------------------------------------------------

        struct ArchiveReadCloser
        {
            void operator()(archive* a) const
            {
                archive_read_free(a);
            }
        };

        typedef std::unique_ptr<archive, ArchiveReadCloser> ReaderPtr;

        ReaderPtr OpenTar(const std::wstring& path)
        {
            ReaderPtr a(archive_read_new());
            archive_read_support_filter_all(a.get());
            archive_read_support_format_tar(a.get());
            archive_read_support_format_gnutar(a.get());

            if (archive_read_open_filename_w(a.get(), path.c_str(), 64 * 1024) != ARCHIVE_OK)
            {
                return ReaderPtr();
            }

            return a;
        }

        std::string EntryName(archive_entry* entry)
        {
            if (const char* utf8 = archive_entry_pathname_utf8(entry))
            {
                return utf8;
            }

            if (const wchar_t* w = archive_entry_pathname_w(entry))
            {
                return ToUtf8(w);
            }

            return std::string();
        }

        Status ExtractTar(const std::string& archivePath, const std::wstring& archiveWide, const std::wstring& rootLong,
            bool overwrite, Progress& progress, Rollback& rb, ExtractSummary& summary, std::string& error)
        {
            // Phase 1: a full pass over the headers (TAR has no central directory).
            std::vector<Item> items;
            {
                ReaderPtr a = OpenTar(archiveWide);
                if (!a)
                {
                    error = archivePath + " is not a ZIP or TAR archive (or it is damaged).";
                    return Status::FileReadError;
                }

                archive_entry* entry = nullptr;
                int rc = ARCHIVE_OK;
                while ((rc = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK || rc == ARCHIVE_WARN)
                {
                    Item item;
                    item.name = EntryName(entry);
                    const unsigned type = archive_entry_filetype(entry);
                    item.isDir = type == AE_IFDIR;

                    // Links (symbolic or hard) and devices are never created.
                    item.skip = archive_entry_hardlink(entry) != nullptr || (type != AE_IFREG && type != AE_IFDIR);
                    item.size = archive_entry_size_is_set(entry) ? static_cast<unsigned long long>(archive_entry_size(entry)) : 0;
                    item.hasMtime = archive_entry_mtime_is_set(entry) != 0;
                    item.mtime = static_cast<long long>(archive_entry_mtime(entry));

                    std::string reason;
                    if (!item.skip && !SafeRelativePath(item.name, item.relative, reason))
                    {
                        error = "Refusing to extract \"" + item.name + "\": " + reason + ". Nothing was extracted.";
                        return Status::UnsafeEntry;
                    }

                    items.push_back(item);
                    archive_read_data_skip(a.get());
                }

                if (rc != ARCHIVE_EOF)
                {
                    const char* why = archive_error_string(a.get());
                    error = items.empty() ? archivePath + " is not a ZIP or TAR archive (or it is damaged)."
                        : "Error reading " + archivePath + ": " + (why ? why : "the archive is damaged");
                    return Status::FileReadError;
                }
            }

            // Phase 2.
            const Status planned = CheckPlan(items, rootLong, overwrite, rb, progress.total, error);
            if (planned != Status::Success)
            {
                return planned;
            }

            // Phase 3: a second pass, writing the data. Entry k of this pass is items[k].
            ReaderPtr a = OpenTar(archiveWide);
            if (!a)
            {
                error = "Cannot reopen " + archivePath;
                return Status::FileReadError;
            }

            archive_entry* entry = nullptr;
            for (const Item& item : items)
            {
                if (archive_read_next_header(a.get(), &entry) < ARCHIVE_WARN)
                {
                    error = "Error reading " + archivePath + ": " + (archive_error_string(a.get()) ? archive_error_string(a.get()) : "?");
                    return Status::FileReadError;
                }

                if (item.skip)
                {
                    summary.skipped.push_back(item.name);
                    continue;
                }

                if (item.relative.empty())
                {
                    continue;
                }

                if (item.isDir)
                {
                    unsigned created = 0;
                    if (!CreateFolders(rootLong + L"\\" + item.relative, rb, created, error))
                    {
                        return Status::ArchiveCreateError;
                    }

                    summary.folders += created;
                    continue;
                }

                archive* reader = a.get();
                const ChunkReader read = [reader](char* buffer, size_t capacity) -> long long
                {
                    return static_cast<long long>(archive_read_data(reader, buffer, capacity));
                };

                const Status result = WriteEntryFile(rootLong, item, read, true /* checked in phase 2 */, rb, progress, summary, error);
                if (result == Status::FileReadError)
                {
                    const char* why = archive_error_string(a.get());
                    error = "\"" + item.name + "\" is damaged: " + (why ? why : "read error");
                }

                if (result != Status::Success)
                {
                    return result;
                }
            }

            return Status::Success;
        }
    } // namespace

    Status ExtractArchive(
        const std::string& archivePath,
        const std::string& destFolder,
        const std::string& password,
        bool overwrite,
        ProgressCallback progress,
        ExtractSummary& summary,
        std::string& errorMessage)
    {
        summary = ExtractSummary();
        errorMessage.clear();

        std::wstring archiveWide, destWide;
        if (!ToWide(archivePath, archiveWide) || archiveWide.empty())
        {
            errorMessage = "The archive path is empty or not valid UTF-8.";
            return Status::InvalidArgument;
        }

        if (!ToWide(destFolder, destWide) || destWide.empty())
        {
            errorMessage = "The destination folder is empty or not valid UTF-8.";
            return Status::InvalidArgument;
        }

        const std::wstring archiveLong = LongPath(FullPath(archiveWide));
        if (!Exists(archiveLong) || IsDirectory(archiveLong))
        {
            errorMessage = "Archive not found: " + archivePath;
            return Status::FileNotFound;
        }

        const std::wstring rootLong = LongPath(FullPath(destWide));
        if (Exists(rootLong) && !IsDirectory(rootLong))
        {
            errorMessage = "The destination is a file, not a folder: " + destFolder;
            return Status::InvalidArgument;
        }

        Rollback rb;
        Progress tracker;
        tracker.callback = progress;
        if (progress && !progress(0.0))
        {
            errorMessage = "Cancelled; nothing was extracted.";
            return Status::Cancelled;
        }

        const Status status = IsZipFile(archiveLong)
            ? ExtractZip(archivePath, rootLong, password, overwrite, tracker, rb, summary, errorMessage)
            : ExtractTar(archivePath, archiveLong, rootLong, overwrite, tracker, rb, summary, errorMessage);

        if (status == Status::Cancelled)
        {
            errorMessage = "Cancelled; the files extracted so far were removed.";
        }

        if (status != Status::Success)
        {
            summary.files = 0;
            summary.folders = 0;
            summary.bytes = 0;
            return status;   // Rollback removes what was created
        }

        rb.committed = true;
        if (progress)
        {
            progress(1.0);
        }

        return Status::Success;
    }
} // namespace ZipEngine
