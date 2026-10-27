// ZipEngineTests.cpp : tests for ZipEngineLib.
//
// Each test creates real archives in its own folder under %TEMP%, then checks them by
// reading them back: ZIP with libzip (entry names, encryption method, contents, password
// handling), TAR with libarchive (names, contents, compression filter). Archives are also
// checked with independent tools: Windows' own tar.exe extracts the TAR variants, and if
// 7-Zip is installed `7z t` verifies ZIP and TAR (the runbook's interoperability check).
//
// Prints PASS/FAIL per test and exits 0 only if every check passed.
// To add a test: write a `void MyTest()` using CHECK(...) and add it to the table in main().

#include "../ZipEngineLib/ZipEngine.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <zip.h>
#include <archive.h>
#include <archive_entry.h>

#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace
{
    int g_checks = 0;
    int g_failures = 0;
    const char* g_test = "";

    // Records a failure (line + expression) and keeps going; also active in Release builds.
#define CHECK(cond)                                                                      \
    do {                                                                                 \
        ++g_checks;                                                                      \
        if (!(cond)) {                                                                   \
            ++g_failures;                                                                \
            std::printf("  FAILED %s (line %d): %s\n", g_test, __LINE__, #cond);         \
        }                                                                                \
    } while (0)

    using ZipEngine::Status;

    // ---- UTF-8 / file helpers ---------------------------------------------------------

    std::wstring Wide(const std::string& utf8)
    {
        const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
        std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
        if (n > 1)
        {
            MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &w[0], n);
        }

        return w;
    }

    std::string Utf8(const std::wstring& w)
    {
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
        if (n > 1)
        {
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
        }

        return s;
    }

    // A fresh folder under %TEMP% for one test (UTF-8 path, no trailing slash).
    std::string NewDir(const char* name)
    {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        wchar_t dir[MAX_PATH];
        swprintf_s(dir, L"%sZipEngineTest_%lu_%llu_%S", tmp, GetCurrentProcessId(), GetTickCount64(), name);
        CreateDirectoryW(dir, nullptr);
        return Utf8(dir);
    }

    void MakeDir(const std::string& path)
    {
        CreateDirectoryW(Wide(path).c_str(), nullptr);
    }

    void WriteFile(const std::string& path, const std::string& data)
    {
        HANDLE h = CreateFileW(Wide(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        if (!data.empty())
        {
            ::WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
        }

        CloseHandle(h);
    }

    std::string ReadFile(const std::string& path)
    {
        HANDLE h = CreateFileW(Wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return std::string();
        }

        std::string data;
        char buf[65536];
        DWORD got = 0;
        while (::ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0)
        {
            data.append(buf, got);
        }

        CloseHandle(h);
        return data;
    }

    bool Exists(const std::string& path) { return GetFileAttributesW(Wide(path).c_str()) != INVALID_FILE_ATTRIBUTES; }

    void RemoveTree(const std::string& path)
    {
        const std::wstring w = Wide(path);
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW((w + L"\\*").c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE)
        {
            do
            {
                const std::wstring name = fd.cFileName;
                if (name == L"." || name == L"..")
                {
                    continue;
                }

                const std::wstring child = w + L"\\" + name;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    RemoveTree(Utf8(child));
                }
                else
                {
                    DeleteFileW(child.c_str());
                }
            } while (FindNextFileW(hFind, &fd));

            FindClose(hFind);
        }

        RemoveDirectoryW(w.c_str());
    }

    // Repeatable, poorly compressible test data.
    std::string Pseudorandom(size_t n, unsigned seed)
    {
        std::string s(n, '\0');
        unsigned x = seed;
        for (size_t i = 0; i < n; ++i)
        {
            x = x * 1103515245u + 12345u;
            s[i] = static_cast<char>(x >> 16);
        }

        return s;
    }

    // ---- Reading archives back with libzip --------------------------------------------

    struct Entry
    {
        std::string    name;
        zip_uint16_t   encryption = ZIP_EM_NONE;
        zip_int32_t    compression = ZIP_CM_STORE;
        zip_uint64_t   size = 0;
    };

    std::vector<Entry> ListEntries(const std::string& zipPath)
    {
        std::vector<Entry> entries;
        int err = 0;
        zip_t* za = zip_open(zipPath.c_str(), ZIP_RDONLY, &err);
        if (!za)
        {
            return entries;
        }

        const zip_int64_t n = zip_get_num_entries(za, 0);
        for (zip_int64_t i = 0; i < n; ++i)
        {
            zip_stat_t st;
            zip_stat_init(&st);
            if (zip_stat_index(za, static_cast<zip_uint64_t>(i), 0, &st) == 0)
            {
                Entry e;
                e.name = st.name;
                e.encryption = st.encryption_method;
                e.compression = static_cast<zip_int32_t>(st.comp_method);
                e.size = st.size;
                entries.push_back(e);
            }
        }

        zip_discard(za);
        return entries;
    }

    // Reads one entry. password == nullptr means "no password". Returns false and sets
    // zipError if libzip refuses (e.g. ZIP_ER_WRONGPASSWD / ZIP_ER_NOPASSWD).
    bool ReadEntry(const std::string& zipPath, const std::string& name, const char* password, std::string& content, int& zipError)
    {
        content.clear();
        zipError = 0;
        int err = 0;
        zip_t* za = zip_open(zipPath.c_str(), ZIP_RDONLY, &err);
        if (!za)
        {
            zipError = err;
            return false;
        }

        const zip_int64_t index = zip_name_locate(za, name.c_str(), ZIP_FL_ENC_UTF_8);
        zip_file_t* zf = index < 0 ? nullptr
            : zip_fopen_index_encrypted(za, static_cast<zip_uint64_t>(index), 0, password);
        if (!zf)
        {
            zipError = zip_error_code_zip(zip_get_error(za));
            zip_discard(za);
            return false;
        }

        char buf[65536];
        zip_int64_t got = 0;
        bool ok = true;
        while ((got = zip_fread(zf, buf, sizeof(buf))) > 0)
        {
            content.append(buf, static_cast<size_t>(got));
        }

        if (got < 0)
        {
            ok = false;
            zipError = zip_error_code_zip(zip_file_get_error(zf));
        }

        zip_fclose(zf);
        zip_discard(za);
        return ok;
    }

    // ---- 7-Zip (optional) --------------------------------------------------------------

    std::wstring SevenZipPath()
    {
        for (const wchar_t* p : { L"C:\\Program Files\\7-Zip\\7z.exe", L"C:\\Program Files (x86)\\7-Zip\\7z.exe" })
        {
            if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES)
            {
                return p;
            }
        }

        return std::wstring();
    }

    // Runs a command line and returns its exit code (-1 if it could not start).
    int Run(std::wstring cmd)
    {
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        {
            return -1;
        }

        WaitForSingleObject(pi.hProcess, 60000);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return static_cast<int>(code);
    }

    // Runs `7z t` (test integrity, which decrypts and checks every CRC). Returns 7-Zip's exit code.
    int SevenZipTest(const std::wstring& exe, const std::string& zipPath, const std::string& password)
    {
        const std::wstring pw = password.empty() ? L"" : L" \"-p" + Wide(password) + L"\"";
        return Run(L"\"" + exe + L"\" t -y -bso0 -bse0 -bsp0" + pw + L" \"" + Wide(zipPath) + L"\"");
    }

    // ---- Reading TAR archives back with libarchive ------------------------------------

    // Reads every entry (name -> content) and reports the outermost compression filter.
    bool ReadTar(const std::string& path, std::map<std::string, std::string>& entries, int& filterCode)
    {
        entries.clear();
        archive* a = archive_read_new();
        archive_read_support_filter_all(a);
        archive_read_support_format_tar(a);
        bool ok = archive_read_open_filename_w(a, Wide(path).c_str(), 65536) == ARCHIVE_OK;
        archive_entry* entry = nullptr;
        while (ok && archive_read_next_header(a, &entry) == ARCHIVE_OK)
        {
            const char* name = archive_entry_pathname_utf8(entry);
            std::string data;
            char buf[65536];
            la_ssize_t got = 0;
            while ((got = archive_read_data(a, buf, sizeof(buf))) > 0)
            {
                data.append(buf, static_cast<size_t>(got));
            }

            ok = got == 0;
            entries[name ? name : "?"] = data;
        }

        filterCode = archive_filter_code(a, 0);
        archive_read_free(a);
        return ok;
    }

    std::string TarExe()
    {
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        const std::wstring exe = std::wstring(sys) + L"\\tar.exe";
        return GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES ? std::string() : Utf8(exe);
    }

    size_t CountFiles(const std::string& dir)
    {
        size_t n = 0;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((Wide(dir) + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
        {
            return 0;
        }

        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                ++n;
            }
        } while (FindNextFileW(h, &fd));

        FindClose(h);
        return n;
    }

    // ---- Tests ------------------------------------------------------------------------

    void EncryptedRoundTrip()
    {
        const std::string dir = NewDir("enc");
        std::string text;   // ~50 KB of compressible text, so Deflate is actually used
        for (int i = 0; i < 2000; ++i)
        {
            text += "Hello, encrypted world!\r\n";
        }

        const std::string binary = Pseudorandom(200000, 7);
        WriteFile(dir + "\\hello.txt", text);
        WriteFile(dir + "\\data.bin", binary);
        const std::string zipPath = dir + "\\out.zip";
        const std::string password = "Correct-Horse-Battery-Staple-42";

        ZipEngine::ZipCompressor zc;
        CHECK(zc.CreateEncryptedZip(zipPath, { dir + "\\hello.txt", dir + "\\data.bin" }, password) == Status::Success);
        CHECK(zc.GetLastErrorMessage().empty());

        const std::vector<Entry> entries = ListEntries(zipPath);
        CHECK(entries.size() == 2);
        for (const Entry& e : entries)
        {
            CHECK(e.encryption == ZIP_EM_AES_256);   // AES-256, not legacy ZipCrypto
        }

        CHECK(entries.size() == 2 && entries[0].name == "hello.txt" && entries[1].name == "data.bin");

        // Text is deflated even when encrypted; random data does not compress, so libzip
        // may store it as-is (it never stores anything larger than the original).
        CHECK(entries.size() == 2 && entries[0].compression == ZIP_CM_DEFLATE);
        CHECK(ReadFile(zipPath).size() < binary.size() + text.size() / 10);

        std::string content;
        int zerr = 0;
        CHECK(ReadEntry(zipPath, "hello.txt", password.c_str(), content, zerr) && content == text);
        CHECK(ReadEntry(zipPath, "data.bin", password.c_str(), content, zerr) && content == binary);

        CHECK(!ReadEntry(zipPath, "hello.txt", "wrong password", content, zerr));
        CHECK(zerr == ZIP_ER_WRONGPASSWD);
        CHECK(!ReadEntry(zipPath, "hello.txt", nullptr, content, zerr));
        CHECK(zerr == ZIP_ER_NOPASSWD);

        // The plaintext must not appear anywhere in the archive file.
        CHECK(ReadFile(zipPath).find("encrypted world") == std::string::npos);

        RemoveTree(dir);
    }

    void UnencryptedWhenNoPassword()
    {
        const std::string dir = NewDir("plain");
        const std::string text(5000, 'A');   // very compressible
        WriteFile(dir + "\\a.txt", text);
        const std::string zipPath = dir + "\\out.zip";

        ZipEngine::ZipCompressor zc;
        CHECK(zc.CreateEncryptedZip(zipPath, { dir + "\\a.txt" }) == Status::Success);   // runbook's 3-argument form
        const std::vector<Entry> entries = ListEntries(zipPath);
        CHECK(entries.size() == 1 && entries[0].encryption == ZIP_EM_NONE);

        std::string content;
        int zerr = 0;
        CHECK(ReadEntry(zipPath, "a.txt", nullptr, content, zerr) && content == text);
        CHECK(ReadFile(zipPath).size() < 1000);   // it really was compressed
        RemoveTree(dir);
    }

    void UnicodeFileNames()
    {
        // UTF-8 file and archive names: "résumé_文件.txt" inside "ärchive.zip".
        const std::string dir = NewDir("unicode");
        const std::string name = "r\xC3\xA9sum\xC3\xA9_\xE6\x96\x87\xE4\xBB\xB6.txt";
        const std::string password = "Any ASCII p@ssword, with spaces & punctuation!";
        WriteFile(dir + "\\" + name, "unicode content");
        const std::string zipPath = dir + "\\\xC3\xA4rchive.zip";

        ZipEngine::ZipCompressor zc;
        CHECK(zc.CreateEncryptedZip(zipPath, { dir + "\\" + name }, password) == Status::Success);
        const std::vector<Entry> entries = ListEntries(zipPath);
        CHECK(entries.size() == 1 && entries[0].name == name);

        std::string content;
        int zerr = 0;
        CHECK(ReadEntry(zipPath, name, password.c_str(), content, zerr) && content == "unicode content");
        RemoveTree(dir);
    }

    void NonAsciiPasswordRejected()
    {
        // 7-Zip decodes ZIP passwords with the local ANSI code page, so non-ASCII passwords
        // are not portable; the engine refuses them rather than create a hard-to-open archive.
        const std::string dir = NewDir("pwrule");
        WriteFile(dir + "\\a.txt", "x");
        const std::string zipPath = dir + "\\out.zip";

        ZipEngine::ZipCompressor zc;
        CHECK(zc.CreateEncryptedZip(zipPath, { dir + "\\a.txt" }, "p\xC3\xA4ssw\xC3\xB6rd") == Status::InvalidArgument);
        CHECK(zc.GetLastErrorMessage().find("English letters") != std::string::npos);
        CHECK(zc.CreateEncryptedZip(zipPath, { dir + "\\a.txt" }, "tab\there") == Status::InvalidArgument);   // control character
        CHECK(!Exists(zipPath));
        CHECK(zc.CreateEncryptedZip(zipPath, { dir + "\\a.txt" }, " !\"#$%&'()*+,-./09:;<=>?@AZ[\\]^_`az{|}~") == Status::Success);
        RemoveTree(dir);
    }

    void EmptyFileIncluded()
    {
        const std::string dir = NewDir("empty");
        WriteFile(dir + "\\empty.txt", "");
        const std::string zipPath = dir + "\\out.zip";

        ZipEngine::ZipCompressor zc;
        CHECK(zc.CreateEncryptedZip(zipPath, { dir + "\\empty.txt" }, "pw") == Status::Success);
        std::string content = "x";
        int zerr = 0;
        CHECK(ReadEntry(zipPath, "empty.txt", "pw", content, zerr) && content.empty());
        RemoveTree(dir);
    }

    void MissingFileLeavesNoArchive()
    {
        const std::string dir = NewDir("missing");
        WriteFile(dir + "\\real.txt", "x");
        const std::string zipPath = dir + "\\out.zip";

        ZipEngine::ZipCompressor zc;
        CHECK(zc.CreateEncryptedZip(zipPath, { dir + "\\real.txt", dir + "\\nope.txt" }, "pw") == Status::FileNotFound);
        CHECK(zc.GetLastErrorMessage().find("nope.txt") != std::string::npos);
        CHECK(!Exists(zipPath));

        MakeDir(dir + "\\folder");
        CHECK(zc.CreateEncryptedZip(zipPath, { dir + "\\folder" }, "pw") == Status::FileNotFound);
        CHECK(!Exists(zipPath));
        RemoveTree(dir);
    }

    void DuplicateNamesRejected()
    {
        // Same name in two folders (and differing only by case) would overwrite each other.
        const std::string dir = NewDir("dup");
        MakeDir(dir + "\\a");
        MakeDir(dir + "\\b");
        WriteFile(dir + "\\a\\report.txt", "first");
        WriteFile(dir + "\\b\\REPORT.TXT", "second");
        const std::string zipPath = dir + "\\out.zip";

        ZipEngine::ZipCompressor zc;
        CHECK(zc.CreateEncryptedZip(zipPath, { dir + "\\a\\report.txt", dir + "\\b\\REPORT.TXT" }, "pw") == Status::DuplicateEntryName);
        CHECK(zc.GetLastErrorMessage().find("REPORT.TXT") != std::string::npos);
        CHECK(!Exists(zipPath));
        RemoveTree(dir);
    }

    void InvalidArguments()
    {
        const std::string dir = NewDir("args");
        WriteFile(dir + "\\a.txt", "x");

        ZipEngine::ZipCompressor zc;
        CHECK(zc.CreateEncryptedZip("", { dir + "\\a.txt" }) == Status::InvalidArgument);
        CHECK(zc.CreateEncryptedZip(dir + "\\out.zip", {}) == Status::InvalidArgument);

        // The archive may not contain itself.
        WriteFile(dir + "\\self.zip", "old");
        CHECK(zc.CreateEncryptedZip(dir + "\\self.zip", { dir + "\\a.txt", dir + "\\self.zip" }) == Status::InvalidArgument);
        CHECK(ReadFile(dir + "\\self.zip") == "old");

        // Output folder must exist.
        CHECK(zc.CreateEncryptedZip(dir + "\\no-such-folder\\out.zip", { dir + "\\a.txt" }) == Status::ArchiveCreateError);
        RemoveTree(dir);
    }

    void LockedFileReported()
    {
        const std::string dir = NewDir("locked");
        WriteFile(dir + "\\locked.txt", "secret");
        HANDLE h = CreateFileW(Wide(dir + "\\locked.txt").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);

        ZipEngine::ZipCompressor zc;
        CHECK(zc.CreateEncryptedZip(dir + "\\out.zip", { dir + "\\locked.txt" }, "pw") == Status::FileReadError);
        CHECK(zc.GetLastErrorMessage().find("locked.txt") != std::string::npos);
        CHECK(!Exists(dir + "\\out.zip"));

        CloseHandle(h);
        RemoveTree(dir);
    }

    void ProgressReportedAndCancelKeepsOldFile()
    {
        const std::string dir = NewDir("progress");
        WriteFile(dir + "\\big.bin", Pseudorandom(3 * 1024 * 1024, 3));
        const std::string zipPath = dir + "\\out.zip";

        // Progress runs from 0 to 1 and never goes backwards.
        std::vector<double> seen;
        ZipEngine::ZipCompressor zc;
        CHECK(zc.CreateEncryptedZip(zipPath, { dir + "\\big.bin" }, "pw",
            [&seen](double p) { seen.push_back(p); return true; }) == Status::Success);
        CHECK(seen.size() >= 3);

        bool monotonic = true;
        for (size_t i = 1; i < seen.size(); ++i)
        {
            monotonic = monotonic && seen[i] >= seen[i - 1];
        }

        CHECK(monotonic);
        CHECK(!seen.empty() && seen.back() == 1.0);

        // Cancelling halfway: Status::Cancelled and the previous archive is untouched.
        const std::string before = ReadFile(zipPath);
        CHECK(zc.CreateEncryptedZip(zipPath, { dir + "\\big.bin" }, "other",
            [](double p) { return p < 0.3; }) == Status::Cancelled);
        CHECK(ReadFile(zipPath) == before);
        RemoveTree(dir);
    }

    void SevenZipCanOpen()
    {
        const std::wstring sevenZip = SevenZipPath();
        if (sevenZip.empty())
        {
            std::printf("  (skipped: 7-Zip is not installed)\n");
            return;
        }

        const std::string dir = NewDir("7z");
        WriteFile(dir + "\\one.txt", "first file");
        WriteFile(dir + "\\two.bin", Pseudorandom(100000, 11));
        const std::string zipPath = dir + "\\out.zip";

        ZipEngine::ZipCompressor zc;
        CHECK(zc.CreateEncryptedZip(zipPath, { dir + "\\one.txt", dir + "\\two.bin" }, "S3cret!pass") == Status::Success);
        CHECK(SevenZipTest(sevenZip, zipPath, "S3cret!pass") == 0);    // decrypts, CRCs match
        CHECK(SevenZipTest(sevenZip, zipPath, "wrong") != 0);          // wrong password rejected

        // Every printable ASCII character must survive the trip to 7-Zip, and a Unicode
        // file name must be readable by it too.
        const std::string allAscii = " !#$%&'()*+,-./09:;<=>?@AZ[\\]^_`az{|}~";   // no '"': it would end 7-Zip's -p argument
        const std::string unicodeName = "r\xC3\xA9sum\xC3\xA9_\xE6\x96\x87\xE4\xBB\xB6.txt";
        WriteFile(dir + "\\" + unicodeName, "unicode");
        CHECK(zc.CreateEncryptedZip(zipPath, { dir + "\\one.txt", dir + "\\" + unicodeName }, allAscii) == Status::Success);
        CHECK(SevenZipTest(sevenZip, zipPath, allAscii) == 0);
        RemoveTree(dir);
    }

    // ---- TAR ------------------------------------------------------------------------

    void TarFormatsRoundTrip()
    {
        using ZipEngine::ArchiveFormat;
        const std::string dir = NewDir("tar");
        std::string text;
        for (int i = 0; i < 4000; ++i)
        {
            text += "line of very compressible tar test text\n";
        }

        const std::string binary = Pseudorandom(150000, 5);
        const std::string unicodeName = "r\xC3\xA9sum\xC3\xA9_\xE6\x96\x87\xE4\xBB\xB6.txt";
        WriteFile(dir + "\\notes.txt", text);
        WriteFile(dir + "\\data.bin", binary);
        WriteFile(dir + "\\" + unicodeName, "unicode content");
        const std::vector<std::string> inputs = { dir + "\\notes.txt", dir + "\\data.bin", dir + "\\" + unicodeName };

        const struct { ArchiveFormat format; const char* ext; int filter; } cases[] = {
            { ArchiveFormat::Tar,    ".tar",     ARCHIVE_FILTER_NONE },
            { ArchiveFormat::TarGz,  ".tar.gz",  ARCHIVE_FILTER_GZIP },
            { ArchiveFormat::TarBz2, ".tar.bz2", ARCHIVE_FILTER_BZIP2 },
            { ArchiveFormat::TarXz,  ".tar.xz",  ARCHIVE_FILTER_XZ },
        };

        const std::string tarExe = TarExe();
        const std::wstring sevenZip = SevenZipPath();

        for (const auto& c : cases)
        {
            const std::string path = dir + "\\out" + c.ext;
            ZipEngine::TarCompressor tc;
            CHECK(tc.CreateTar(path, inputs, c.format) == Status::Success);

            std::map<std::string, std::string> entries;
            int filter = -1;
            CHECK(ReadTar(path, entries, filter));
            CHECK(filter == c.filter);   // really gzip / bzip2 / xz, not just named so
            CHECK(entries.size() == 3);
            CHECK(entries["notes.txt"] == text);
            CHECK(entries["data.bin"] == binary);
            CHECK(entries[unicodeName] == "unicode content");   // UTF-8 name via pax header

            const size_t size = ReadFile(path).size();
            if (c.format == ArchiveFormat::Tar)
            {
                CHECK(size >= text.size() + binary.size());
            }
            else
            {
                CHECK(size < binary.size() + text.size() / 10);   // the text really was compressed
            }

            // Windows' own tar.exe (bsdtar) must extract identical files. It converts names to
            // the system ANSI code page and cannot handle characters outside it (it skips the
            // CJK name above, and crashes if asked to create such an archive itself), so this
            // check uses an accented Latin name that every Western code page can represent.
            if (!tarExe.empty())
            {
                const std::string accentName = "accent_r\xC3\xA9sum\xC3\xA9.txt";
                WriteFile(dir + "\\" + accentName, "accented");
                const std::string winPath = dir + "\\wintar" + c.ext;
                CHECK(tc.CreateTar(winPath, { dir + "\\notes.txt", dir + "\\data.bin", dir + "\\" + accentName }, c.format) == Status::Success);

                const std::string outDir = dir + "\\x" + std::string(c.ext).substr(1);
                MakeDir(outDir);
                CHECK(Run(L"\"" + Wide(tarExe) + L"\" -xf \"" + Wide(winPath) + L"\" -C \"" + Wide(outDir) + L"\"") == 0);
                CHECK(ReadFile(outDir + "\\notes.txt") == text);
                CHECK(ReadFile(outDir + "\\data.bin") == binary);
                CHECK(ReadFile(outDir + "\\" + accentName) == "accented");
            }

            if (!sevenZip.empty())
            {
                CHECK(SevenZipTest(sevenZip, path, "") == 0);
            }
        }

        if (tarExe.empty())
        {
            std::printf("  (tar.exe not found: extraction check skipped)\n");
        }

        RemoveTree(dir);
    }

    void TarRulesMatchZip()
    {
        using ZipEngine::ArchiveFormat;
        const std::string dir = NewDir("tarrules");
        MakeDir(dir + "\\a");
        MakeDir(dir + "\\b");
        WriteFile(dir + "\\a\\same.txt", "1");
        WriteFile(dir + "\\b\\same.txt", "2");
        WriteFile(dir + "\\ok.txt", "ok");
        const std::string out = dir + "\\out.tar.gz";

        ZipEngine::TarCompressor tc;
        CHECK(tc.CreateTar(out, { dir + "\\ok.txt", dir + "\\missing.txt" }) == Status::FileNotFound);
        CHECK(tc.CreateTar(out, { dir + "\\a\\same.txt", dir + "\\b\\same.txt" }) == Status::DuplicateEntryName);
        CHECK(tc.CreateTar(out, { dir + "\\a" }) == Status::FileNotFound);   // folder
        CHECK(tc.CreateTar(out, { dir + "\\ok.txt" }, ArchiveFormat::Zip) == Status::InvalidArgument);
        CHECK(!Exists(out));

        // A password with a TAR format is refused, not silently ignored.
        std::string error;
        CHECK(ZipEngine::CreateArchive(ArchiveFormat::TarGz, out, { dir + "\\ok.txt" }, "secret", nullptr, error) == Status::InvalidArgument);
        CHECK(error.find("cannot be encrypted") != std::string::npos);
        CHECK(!Exists(out));

        // Only the input files remain: no temporary files were left behind.
        CHECK(CountFiles(dir) == 1);
        RemoveTree(dir);
    }

    void TarCancelKeepsOldFile()
    {
        const std::string dir = NewDir("tarcancel");
        WriteFile(dir + "\\big.bin", Pseudorandom(4 * 1024 * 1024, 9));
        const std::string out = dir + "\\out.tar.xz";
        WriteFile(out, "previous archive");

        std::vector<double> seen;
        ZipEngine::TarCompressor tc;
        CHECK(tc.CreateTar(out, { dir + "\\big.bin" }, ZipEngine::ArchiveFormat::TarXz,
            [&seen](double p) { seen.push_back(p); return p < 0.5; }) == Status::Cancelled);
        CHECK(ReadFile(out) == "previous archive");
        CHECK(CountFiles(dir) == 2);   // big.bin + the untouched old archive; no temp file
        CHECK(!seen.empty() && seen.back() >= 0.5);

        // And a normal run replaces it, reporting progress up to 1.0.
        seen.clear();
        CHECK(tc.CreateTar(out, { dir + "\\big.bin" }, ZipEngine::ArchiveFormat::TarXz,
            [&seen](double p) { seen.push_back(p); return true; }) == Status::Success);
        CHECK(!seen.empty() && seen.back() == 1.0);
        CHECK(ReadFile(out) != "previous archive");
        RemoveTree(dir);
    }

    void FormatDetection()
    {
        using ZipEngine::ArchiveFormat;
        ArchiveFormat f = ArchiveFormat::Zip;
        CHECK(ZipEngine::FormatFromPath("C:\\x\\a.ZIP", f) && f == ArchiveFormat::Zip);
        CHECK(ZipEngine::FormatFromPath("a.tar", f) && f == ArchiveFormat::Tar);
        CHECK(ZipEngine::FormatFromPath("a.tar.gz", f) && f == ArchiveFormat::TarGz);
        CHECK(ZipEngine::FormatFromPath("a.TGZ", f) && f == ArchiveFormat::TarGz);
        CHECK(ZipEngine::FormatFromPath("a.tar.bz2", f) && f == ArchiveFormat::TarBz2);
        CHECK(ZipEngine::FormatFromPath("a.tbz2", f) && f == ArchiveFormat::TarBz2);
        CHECK(ZipEngine::FormatFromPath("a.tar.xz", f) && f == ArchiveFormat::TarXz);
        CHECK(ZipEngine::FormatFromPath("a.txz", f) && f == ArchiveFormat::TarXz);
        CHECK(!ZipEngine::FormatFromPath("a.gz", f));     // a bare .gz is not a tar archive
        CHECK(!ZipEngine::FormatFromPath("a.7z", f));
        CHECK(ZipEngine::FormatFromName("TAR.GZ", f) && f == ArchiveFormat::TarGz);
        CHECK(ZipEngine::FormatFromName("xz", f) && f == ArchiveFormat::TarXz);
        CHECK(!ZipEngine::FormatFromName("rar", f));
        CHECK(std::string(ZipEngine::FormatExtension(ArchiveFormat::TarBz2)) == ".tar.bz2");
        CHECK(ZipEngine::FormatSupportsEncryption(ArchiveFormat::Zip));
        CHECK(!ZipEngine::FormatSupportsEncryption(ArchiveFormat::TarGz));
    }

    void ListArchiveWorks()
    {
        const std::string dir = NewDir("list");
        WriteFile(dir + "\\one.txt", "12345");
        WriteFile(dir + "\\two.txt", "1234567890");

        std::string error;
        CHECK(ZipEngine::CreateArchive(ZipEngine::ArchiveFormat::Zip, dir + "\\enc.zip", { dir + "\\one.txt", dir + "\\two.txt" },
            "pw", nullptr, error) == Status::Success);
        CHECK(ZipEngine::CreateArchive(ZipEngine::ArchiveFormat::TarBz2, dir + "\\t.tar.bz2", { dir + "\\one.txt", dir + "\\two.txt" },
            "", nullptr, error) == Status::Success);

        std::vector<ZipEngine::ArchiveEntryInfo> entries;
        CHECK(ZipEngine::ListArchive(dir + "\\enc.zip", entries, error) == Status::Success);
        CHECK(entries.size() == 2 && entries[0].name == "one.txt" && entries[0].encrypted && entries[1].encrypted);

        CHECK(ZipEngine::ListArchive(dir + "\\t.tar.bz2", entries, error) == Status::Success);
        CHECK(entries.size() == 2 && entries[1].name == "two.txt" && entries[1].size == 10 && !entries[1].encrypted);

        CHECK(ZipEngine::ListArchive(dir + "\\one.txt", entries, error) == Status::FileReadError);   // not an archive
        CHECK(ZipEngine::ListArchive(dir + "\\nope.zip", entries, error) == Status::FileNotFound);
        RemoveTree(dir);
    }

    struct Test
    {
        const char* name;
        std::function<void()> fn;
    };
}

int main()
{
    const Test tests[] =
    {
        { "EncryptedRoundTrip", EncryptedRoundTrip },
        { "UnencryptedWhenNoPassword", UnencryptedWhenNoPassword },
        { "UnicodeFileNames", UnicodeFileNames },
        { "NonAsciiPasswordRejected", NonAsciiPasswordRejected },
        { "EmptyFileIncluded", EmptyFileIncluded },
        { "MissingFileLeavesNoArchive", MissingFileLeavesNoArchive },
        { "DuplicateNamesRejected", DuplicateNamesRejected },
        { "InvalidArguments", InvalidArguments },
        { "LockedFileReported", LockedFileReported },
        { "ProgressReportedAndCancelKeepsOldFile", ProgressReportedAndCancelKeepsOldFile },
        { "SevenZipCanOpen", SevenZipCanOpen },
        { "TarFormatsRoundTrip", TarFormatsRoundTrip },
        { "TarRulesMatchZip", TarRulesMatchZip },
        { "TarCancelKeepsOldFile", TarCancelKeepsOldFile },
        { "FormatDetection", FormatDetection },
        { "ListArchiveWorks", ListArchiveWorks },
    };

    std::printf("libzip %s, %s\n\n", zip_libzip_version(), archive_version_details());
    for (const Test& t : tests)
    {
        g_test = t.name;
        const int before = g_failures;
        try
        {
            t.fn();
        }
        catch (const std::exception& ex)
        {
            ++g_failures;
            std::printf("  FAILED %s: unexpected exception: %s\n", t.name, ex.what());
        }

        std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", t.name);
    }

    // Exit code 0 = all passed (used by CI).
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
