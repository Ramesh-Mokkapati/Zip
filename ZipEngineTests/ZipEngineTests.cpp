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

    bool Exists(const std::string& path)
    {
        return GetFileAttributesW(Wide(path).c_str()) != INVALID_FILE_ATTRIBUTES;
    }

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

    // Runs a command line and returns its exit code: -1 if it could not start, -2 if it did not
    // finish within 60 s (it is then killed, so a hung tool cannot outlive the test run).
    int Run(std::wstring cmd)
    {
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        {
            return -1;
        }

        int result = -2;
        if (WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0)
        {
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            result = static_cast<int>(code);
        }
        else
        {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
        }

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return result;
    }

    // Runs a command line and returns what it wrote to stdout (empty if it could not run).
    std::string RunCapture(std::wstring cmd)
    {
        SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
        HANDLE readPipe = nullptr, writePipe = nullptr;
        if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
        {
            return std::string();
        }

        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si = { sizeof(si) };
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = writePipe;
        si.hStdError = writePipe;
        PROCESS_INFORMATION pi = {};
        const BOOL started = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(writePipe);   // so ReadFile sees end-of-file when the child exits

        std::string output;
        if (started)
        {
            char buf[4096];
            DWORD got = 0;
            while (::ReadFile(readPipe, buf, sizeof(buf), &got, nullptr) && got > 0)
            {
                output.append(buf, got);
            }

            WaitForSingleObject(pi.hProcess, 10000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }

        CloseHandle(readPipe);
        return output;
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

    // Whether this tar.exe can decompress the format. `tar --version` lists the libraries it
    // was built with, e.g. "bsdtar 3.8.9 - libarchive 3.8.9 zlib/... liblzma/... bz2lib/...".
    // Windows 10 and Windows Server 2019/2022 ship an older bsdtar without liblzma; given a
    // .tar.xz it tries to run an external "xz" program and can hang, so such checks are skipped.
    bool TarExeSupports(const std::string& versionText, ZipEngine::ArchiveFormat format)
    {
        switch (format)
        {

        case ZipEngine::ArchiveFormat::TarGz:
            return versionText.find("zlib/") != std::string::npos;

        case ZipEngine::ArchiveFormat::TarBz2:
            return versionText.find("bz2lib/") != std::string::npos;

        case ZipEngine::ArchiveFormat::TarXz:
            return versionText.find("liblzma/") != std::string::npos;

        default:
            return true;   // plain .tar needs no decompressor
        }
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

    // Encryption method of every entry of a ZIP (ZIP_EM_*), read back with libzip.
    std::vector<int> EncryptionMethods(const std::string& path)
    {
        std::vector<int> methods;
        int err = 0;
        zip_t* za = zip_open(path.c_str(), ZIP_RDONLY, &err);
        if (!za)
        {
            return methods;
        }

        for (zip_int64_t i = 0; i < zip_get_num_entries(za, 0); ++i)
        {
            zip_stat_t st;
            zip_stat_init(&st);
            if (zip_stat_index(za, static_cast<zip_uint64_t>(i), 0, &st) == 0 && (st.valid & ZIP_STAT_ENCRYPTION_METHOD))
            {
                methods.push_back(st.encryption_method);
            }
        }

        zip_close(za);
        return methods;
    }

    // The legacy option writes ZipCrypto (what Windows Explorer can open); the default stays AES.
    void ZipCryptoLegacyOption()
    {
        const std::string dir = NewDir("zipcrypto");
        const std::string secret = "legacy-plaintext-marker-1234567890";
        WriteFile(dir + "\\a.txt", secret);
        WriteFile(dir + "\\b.bin", Pseudorandom(50000, 5));
        const std::vector<std::string> files = { dir + "\\a.txt", dir + "\\b.bin" };
        std::string error;

        const std::string legacy = dir + "\\legacy.zip";
        CHECK(ZipEngine::CreateArchive(ZipEngine::ArchiveFormat::Zip, legacy, files, "Old-Pass-1",
            ZipEngine::ZipEncryption::ZipCrypto, nullptr, error) == Status::Success);
        CHECK(EncryptionMethods(legacy) == std::vector<int>({ ZIP_EM_TRAD_PKWARE, ZIP_EM_TRAD_PKWARE }));
        CHECK(ReadFile(legacy).find(secret) == std::string::npos);   // contents really are encrypted

        ZipEngine::ExtractSummary summary;
        CHECK(ZipEngine::ExtractArchive(legacy, dir + "\\bad", "wrong", false, nullptr, summary, error) == Status::WrongPassword);
        CHECK(!Exists(dir + "\\bad"));
        CHECK(ZipEngine::ExtractArchive(legacy, dir + "\\out", "Old-Pass-1", false, nullptr, summary, error) == Status::Success);
        CHECK(ReadFile(dir + "\\out\\a.txt") == secret);
        CHECK(ReadFile(dir + "\\out\\b.bin") == Pseudorandom(50000, 5));

        // The old overload and the explicit default both mean AES-256.
        const std::string modern = dir + "\\modern.zip";
        CHECK(ZipEngine::CreateArchive(ZipEngine::ArchiveFormat::Zip, modern, files, "New-Pass-1", nullptr, error) == Status::Success);
        CHECK(EncryptionMethods(modern) == std::vector<int>({ ZIP_EM_AES_256, ZIP_EM_AES_256 }));

        // Without a password the choice is irrelevant: nothing is encrypted.
        const std::string plain = dir + "\\plain.zip";
        CHECK(ZipEngine::CreateArchive(ZipEngine::ArchiveFormat::Zip, plain, files, "",
            ZipEngine::ZipEncryption::ZipCrypto, nullptr, error) == Status::Success);
        CHECK(EncryptionMethods(plain) == std::vector<int>({ ZIP_EM_NONE, ZIP_EM_NONE }));

        const std::wstring sevenZip = SevenZipPath();
        if (!sevenZip.empty())
        {
            CHECK(SevenZipTest(sevenZip, legacy, "Old-Pass-1") == 0);
            CHECK(SevenZipTest(sevenZip, legacy, "wrong") != 0);
        }

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

        const struct { ArchiveFormat format; const char* ext; int filter; } cases[] =
        {
            { ArchiveFormat::Tar,    ".tar",     ARCHIVE_FILTER_NONE },
            { ArchiveFormat::TarGz,  ".tar.gz",  ARCHIVE_FILTER_GZIP },
            { ArchiveFormat::TarBz2, ".tar.bz2", ARCHIVE_FILTER_BZIP2 },
            { ArchiveFormat::TarXz,  ".tar.xz",  ARCHIVE_FILTER_XZ },
        };

        const std::string tarExe = TarExe();
        const std::string tarVersion = tarExe.empty() ? std::string() : RunCapture(L"\"" + Wide(tarExe) + L"\" --version");
        if (!tarVersion.empty())
        {
            std::printf("  (Windows tar.exe: %s)\n", tarVersion.substr(0, tarVersion.find_first_of("\r\n")).c_str());
        }

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
            // Formats this tar.exe was built without (e.g. xz on Windows 10 / Server 2022) are skipped.
            if (!tarExe.empty() && !TarExeSupports(tarVersion, c.format))
            {
                std::printf("  (tar.exe cannot decompress %s on this Windows version: extraction check skipped)\n", c.ext);
            }
            else if (!tarExe.empty())
            {
                const std::string accentName = "accent_r\xC3\xA9sum\xC3\xA9.txt";
                WriteFile(dir + "\\" + accentName, "accented");
                const std::string winPath = dir + "\\wintar" + c.ext;
                CHECK(tc.CreateTar(winPath, { dir + "\\notes.txt", dir + "\\data.bin", dir + "\\" + accentName }, c.format) == Status::Success);

                const std::string outDir = dir + "\\x" + std::string(c.ext).substr(1);
                MakeDir(outDir);
                const int code = Run(L"\"" + Wide(tarExe) + L"\" -xf \"" + Wide(winPath) + L"\" -C \"" + Wide(outDir) + L"\"");
                if (code != 0)
                {
                    std::printf("  tar.exe -xf %s returned %d%s\n", c.ext, code, code == -2 ? " (timed out, killed)" : "");
                }

                CHECK(code == 0);
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

    // The tar.exe capability probe used above: the old bsdtar in Windows 10 / Server 2022 has
    // no liblzma, so its .tar.xz check must be skipped (it hangs instead of failing).
    void TarExeCapabilityProbe()
    {
        using ZipEngine::ArchiveFormat;
        const std::string oldTar = "bsdtar 3.5.2 - libarchive 3.5.2 zlib/1.2.5.f-ipp bz2lib/1.0.6";
        const std::string newTar = "bsdtar 3.8.9 - libarchive 3.8.9 zlib/1.2.13.1-motley liblzma/5.8.3 bz2lib/1.0.8";
        CHECK(TarExeSupports(oldTar, ArchiveFormat::Tar));
        CHECK(TarExeSupports(oldTar, ArchiveFormat::TarGz));
        CHECK(TarExeSupports(oldTar, ArchiveFormat::TarBz2));
        CHECK(!TarExeSupports(oldTar, ArchiveFormat::TarXz));
        CHECK(TarExeSupports(newTar, ArchiveFormat::TarXz));
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

    // ---- Extraction -----------------------------------------------------------------

    // Builds a ZIP with arbitrary entry names (including hostile ones the engine would never
    // write) straight through libzip. Names ending in '/' become folder entries.
    struct RawEntry
    {
        std::string name;
        std::string content;
        unsigned    unixMode = 0;   // e.g. 0120777 marks a symbolic link
    };

    bool MakeRawZip(const std::string& path, const std::vector<RawEntry>& entries)
    {
        int err = 0;
        zip_t* za = zip_open(path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
        if (!za)
        {
            return false;
        }

        for (const RawEntry& e : entries)
        {
            zip_int64_t idx = -1;
            if (!e.name.empty() && e.name.back() == '/')
            {
                idx = zip_dir_add(za, e.name.c_str(), ZIP_FL_ENC_UTF_8);
            }
            else
            {
                // libzip copies nothing until zip_close, so the data must outlive this call:
                // keep a heap copy that libzip frees (freep = 1).
                char* copy = static_cast<char*>(malloc(e.content.size() + 1));
                memcpy(copy, e.content.data(), e.content.size());
                zip_source_t* src = zip_source_buffer(za, copy, e.content.size(), 1);
                idx = zip_file_add(za, e.name.c_str(), src, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE);
            }

            if (idx >= 0 && e.unixMode)
            {
                zip_file_set_external_attributes(za, static_cast<zip_uint64_t>(idx), 0, ZIP_OPSYS_UNIX, static_cast<zip_uint32_t>(e.unixMode) << 16);
            }
        }

        return zip_close(za) == 0;
    }

    // Builds a .tar with arbitrary names and entry types through libarchive.
    struct RawTarEntry
    {
        std::string name;
        std::string content;
        unsigned    type = AE_IFREG;    // AE_IFREG, AE_IFDIR, AE_IFLNK
        std::string linkTarget;         // symlink target (AE_IFLNK) or hard link target (hardlink = true)
        bool        hardlink = false;
    };

    bool MakeRawTar(const std::string& path, const std::vector<RawTarEntry>& entries)
    {
        archive* a = archive_write_new();
        archive_write_set_format_pax_restricted(a);
        if (archive_write_open_filename_w(a, Wide(path).c_str()) != ARCHIVE_OK)
        {
            archive_write_free(a);
            return false;
        }

        for (const RawTarEntry& e : entries)
        {
            archive_entry* entry = archive_entry_new();
            archive_entry_set_pathname_utf8(entry, e.name.c_str());
            archive_entry_set_filetype(entry, e.type);
            archive_entry_set_perm(entry, e.type == AE_IFDIR ? 0755 : 0644);
            if (e.type == AE_IFLNK)
            {
                archive_entry_set_symlink_utf8(entry, e.linkTarget.c_str());
            }

            if (e.hardlink)
            {
                archive_entry_set_hardlink_utf8(entry, e.linkTarget.c_str());
            }

            const bool hasData = e.type == AE_IFREG && !e.hardlink;
            archive_entry_set_size(entry, hasData ? static_cast<la_int64_t>(e.content.size()) : 0);
            archive_write_header(a, entry);
            if (hasData && !e.content.empty())
            {
                archive_write_data(a, e.content.data(), e.content.size());
            }

            archive_entry_free(entry);
        }

        archive_write_close(a);
        archive_write_free(a);
        return true;
    }

    ZipEngine::Status Extract(const std::string& archive, const std::string& dest, const std::string& password,
        bool overwrite, ZipEngine::ExtractSummary& summary, std::string& error,
        ZipEngine::ProgressCallback progress = nullptr)
    {
        return ZipEngine::ExtractArchive(archive, dest, password, overwrite, progress, summary, error);
    }

    void ExtractRoundTripAllFormats()
    {
        using ZipEngine::ArchiveFormat;
        const std::string dir = NewDir("xround");
        const std::string text = "round trip text\r\n";
        const std::string binary = Pseudorandom(300000, 21);
        const std::string unicodeName = "r\xC3\xA9sum\xC3\xA9_\xE6\x96\x87\xE4\xBB\xB6.txt";
        MakeDir(dir + "\\in");
        WriteFile(dir + "\\in\\notes.txt", text);
        WriteFile(dir + "\\in\\data.bin", binary);
        WriteFile(dir + "\\in\\" + unicodeName, "unicode");
        const std::vector<std::string> inputs = { dir + "\\in\\notes.txt", dir + "\\in\\data.bin", dir + "\\in\\" + unicodeName };

        const struct { ArchiveFormat format; const char* ext; const char* password; } cases[] = {
            { ArchiveFormat::Zip,    ".zip",     "Round-Trip-1" },
            { ArchiveFormat::Zip,    ".zip",     "" },
            { ArchiveFormat::Tar,    ".tar",     "" },
            { ArchiveFormat::TarGz,  ".tar.gz",  "" },
            { ArchiveFormat::TarBz2, ".tar.bz2", "" },
            { ArchiveFormat::TarXz,  ".tar.xz",  "" },
        };

        int n = 0;
        for (const auto& c : cases)
        {
            const std::string archive = dir + "\\a" + std::to_string(n) + c.ext;
            const std::string dest = dir + "\\out" + std::to_string(n++);
            std::string error;
            CHECK(ZipEngine::CreateArchive(c.format, archive, inputs, c.password, nullptr, error) == Status::Success);

            ZipEngine::ExtractSummary summary;
            CHECK(Extract(archive, dest, c.password, false, summary, error) == Status::Success);
            CHECK(summary.files == 3 && summary.skipped.empty());
            CHECK(summary.bytes == text.size() + binary.size() + 7);
            CHECK(ReadFile(dest + "\\notes.txt") == text);
            CHECK(ReadFile(dest + "\\data.bin") == binary);
            CHECK(ReadFile(dest + "\\" + unicodeName) == "unicode");
            CHECK(CountFiles(dest) == 3);   // no ".~part" leftovers

            // The original modification time is restored (to within the 2 s ZIP resolution).
            WIN32_FILE_ATTRIBUTE_DATA a = {}, b = {};
            GetFileAttributesExW(Wide(dir + "\\in\\data.bin").c_str(), GetFileExInfoStandard, &a);
            GetFileAttributesExW(Wide(dest + "\\data.bin").c_str(), GetFileExInfoStandard, &b);
            const auto ticks = [](const FILETIME& f) { return (static_cast<long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
            CHECK(llabs(ticks(a.ftLastWriteTime) - ticks(b.ftLastWriteTime)) <= 2 * 10000000LL);
        }

        RemoveTree(dir);
    }

    void ExtractZipPasswords()
    {
        const std::string dir = NewDir("xpw");
        WriteFile(dir + "\\secret.txt", "top secret");
        const std::string archive = dir + "\\s.zip";
        std::string error;
        CHECK(ZipEngine::CreateArchive(ZipEngine::ArchiveFormat::Zip, archive, { dir + "\\secret.txt" }, "Right-Pass-9", nullptr, error) == Status::Success);

        ZipEngine::ExtractSummary summary;
        CHECK(Extract(archive, dir + "\\out", "", false, summary, error) == Status::PasswordRequired);
        CHECK(!Exists(dir + "\\out"));   // nothing created, not even the folder
        CHECK(Extract(archive, dir + "\\out", "wrong", false, summary, error) == Status::WrongPassword);
        CHECK(!Exists(dir + "\\out"));
        CHECK(Extract(archive, dir + "\\out", "Right-Pass-9", false, summary, error) == Status::Success);
        CHECK(ReadFile(dir + "\\out\\secret.txt") == "top secret");

        // A password given for an unencrypted archive is simply not needed.
        CHECK(ZipEngine::CreateArchive(ZipEngine::ArchiveFormat::Zip, dir + "\\p.zip", { dir + "\\secret.txt" }, "", nullptr, error) == Status::Success);
        CHECK(Extract(dir + "\\p.zip", dir + "\\out2", "unused", false, summary, error) == Status::Success);
        RemoveTree(dir);
    }

    void ExtractRefusesUnsafeEntries()
    {
        const std::string dir = NewDir("xunsafe");
        const char* hostile[] = {
            "../evil.txt",            // classic Zip Slip
            "..\\evil.txt",           // same, Windows separator
            "ok/../../evil.txt",      // escapes after a harmless-looking prefix
            "/evil.txt",              // absolute (Unix style)
            "\\evil.txt",             // absolute (Windows style)
            "C:/evil.txt",            // drive letter
            "C:evil.txt",             // drive-relative
            "notes.txt:hidden",       // alternate data stream
            "CON",                    // device name
            "nul.txt",                // device name with an extension
            "sub/LPT1.log",           // device name in a sub-folder
            "trailing.",              // Windows would strip the dot
            "bad|name.txt",           // forbidden character
        };

        int n = 0;
        for (const char* name : hostile)
        {
            // Each hostile name hides behind a harmless entry, which must not be written either.
            const std::string zipPath = dir + "\\h" + std::to_string(n) + ".zip";
            const std::string tarPath = dir + "\\h" + std::to_string(n) + ".tar";
            const std::string dest = dir + "\\dest" + std::to_string(n++);

            CHECK(MakeRawZip(zipPath, { { "harmless.txt", "fine" }, { name, "payload" } }));
            CHECK(MakeRawTar(tarPath, { { "harmless.txt", "fine" }, { name, "payload" } }));

            for (const std::string& archive : { zipPath, tarPath })
            {
                ZipEngine::ExtractSummary summary;
                std::string error;
                const Status status = Extract(archive, dest, "", true, summary, error);
                if (status != Status::UnsafeEntry)
                {
                    std::printf("  hostile entry \"%s\" in %s gave status %d\n", name, archive.c_str(), static_cast<int>(status));
                }

                CHECK(status == Status::UnsafeEntry);
                CHECK(error.find("Nothing was extracted") != std::string::npos);
                CHECK(!Exists(dest));                       // nothing written at all
            }
        }

        CHECK(!Exists(dir + "\\evil.txt"));
        CHECK(!Exists(dir + "\\..\\evil.txt"));
        CHECK(!Exists("C:\\evil.txt"));

        // Duplicate targets (differing only in case on Windows) are refused too.
        CHECK(MakeRawZip(dir + "\\dup.zip", { { "a.txt", "1" }, { "A.TXT", "2" } }));
        ZipEngine::ExtractSummary summary;
        std::string error;
        CHECK(Extract(dir + "\\dup.zip", dir + "\\dupout", "", false, summary, error) == Status::UnsafeEntry);
        RemoveTree(dir);
    }

    void ExtractSkipsLinks()
    {
        const std::string dir = NewDir("xlinks");

        // A symlink pointing at a system folder, then a file "through" it: a classic way to
        // redirect writes. The link must never be created, so the file lands in the destination.
        CHECK(MakeRawTar(dir + "\\links.tar", {
            { "link", "", AE_IFLNK, "C:\\Windows" },
            { "hard", "", AE_IFREG, "normal.txt", true },
            { "normal.txt", "normal" },
            }));

        ZipEngine::ExtractSummary summary;
        std::string error;
        CHECK(Extract(dir + "\\links.tar", dir + "\\out", "", false, summary, error) == Status::Success);
        CHECK(summary.files == 1 && summary.skipped.size() == 2);
        CHECK(!Exists(dir + "\\out\\link") && !Exists(dir + "\\out\\hard"));
        CHECK(ReadFile(dir + "\\out\\normal.txt") == "normal");

        // ZIP made on Unix with a symbolic-link entry.
        CHECK(MakeRawZip(dir + "\\links.zip", { { "ln", "/etc/passwd", 0120777 }, { "real.txt", "real" } }));
        CHECK(Extract(dir + "\\links.zip", dir + "\\out2", "", false, summary, error) == Status::Success);
        CHECK(summary.files == 1 && summary.skipped.size() == 1 && summary.skipped[0] == "ln");
        CHECK(!Exists(dir + "\\out2\\ln"));
        RemoveTree(dir);
    }

    void ExtractFoldersFromOtherTools()
    {
        // Archives from other tools usually contain folders, "./" prefixes and empty folders.
        const std::string dir = NewDir("xfolders");
        CHECK(MakeRawZip(dir + "\\tree.zip", {
            { "project/", "" }, { "project/src/main.c", "int main(){}" }, { "project/docs/", "" },
            { "project\\win\\style.txt", "backslashes" }, { "./top.txt", "top" },
            }));

        ZipEngine::ExtractSummary summary;
        std::string error;
        CHECK(Extract(dir + "\\tree.zip", dir + "\\out", "", false, summary, error) == Status::Success);
        CHECK(ReadFile(dir + "\\out\\project\\src\\main.c") == "int main(){}");
        CHECK(ReadFile(dir + "\\out\\project\\win\\style.txt") == "backslashes");
        CHECK(ReadFile(dir + "\\out\\top.txt") == "top");
        CHECK(GetFileAttributesW(Wide(dir + "\\out\\project\\docs").c_str()) & FILE_ATTRIBUTE_DIRECTORY);   // empty folder kept
        CHECK(summary.files == 3);

        CHECK(MakeRawTar(dir + "\\tree.tar", { { "./pkg/", "", AE_IFDIR }, { "./pkg/a.txt", "A" }, { "pkg/b/c.txt", "C" } }));
        CHECK(Extract(dir + "\\tree.tar", dir + "\\out2", "", false, summary, error) == Status::Success);
        CHECK(ReadFile(dir + "\\out2\\pkg\\a.txt") == "A" && ReadFile(dir + "\\out2\\pkg\\b\\c.txt") == "C");
        RemoveTree(dir);
    }

    void ExtractExistingFiles()
    {
        const std::string dir = NewDir("xexist");
        WriteFile(dir + "\\a.txt", "new A");
        WriteFile(dir + "\\b.txt", "new B");
        std::string error;
        CHECK(ZipEngine::CreateArchive(ZipEngine::ArchiveFormat::TarGz, dir + "\\t.tgz", { dir + "\\a.txt", dir + "\\b.txt" }, "", nullptr, error) == Status::Success);

        MakeDir(dir + "\\out");
        WriteFile(dir + "\\out\\b.txt", "old B");
        ZipEngine::ExtractSummary summary;
        CHECK(Extract(dir + "\\t.tgz", dir + "\\out", "", false, summary, error) == Status::FileExists);
        CHECK(error.find("b.txt") != std::string::npos);
        CHECK(ReadFile(dir + "\\out\\b.txt") == "old B");
        CHECK(!Exists(dir + "\\out\\a.txt"));   // nothing was written

        CHECK(Extract(dir + "\\t.tgz", dir + "\\out", "", true, summary, error) == Status::Success);
        CHECK(ReadFile(dir + "\\out\\a.txt") == "new A" && ReadFile(dir + "\\out\\b.txt") == "new B");
        RemoveTree(dir);
    }

    void ExtractCancelRollsBack()
    {
        const std::string dir = NewDir("xcancel");
        WriteFile(dir + "\\first.bin", Pseudorandom(2 * 1024 * 1024, 31));
        WriteFile(dir + "\\second.bin", Pseudorandom(2 * 1024 * 1024, 32));
        std::string error;
        CHECK(ZipEngine::CreateArchive(ZipEngine::ArchiveFormat::Zip, dir + "\\big.zip", { dir + "\\first.bin", dir + "\\second.bin" }, "Cancel-Me-1", nullptr, error) == Status::Success);

        // An existing destination with a file of its own: that file must survive the rollback.
        MakeDir(dir + "\\out");
        WriteFile(dir + "\\out\\mine.txt", "keep me");
        ZipEngine::ExtractSummary summary;

        CHECK(Extract(dir + "\\big.zip", dir + "\\out", "Cancel-Me-1", false, summary, error,
            [](double p) { return p < 0.7; }) == Status::Cancelled);
        CHECK(ReadFile(dir + "\\out\\mine.txt") == "keep me");
        CHECK(CountFiles(dir + "\\out") == 1);   // first.bin (complete) and the partial second.bin removed

        // A destination created by the cancelled run is removed entirely.
        CHECK(Extract(dir + "\\big.zip", dir + "\\fresh\\nested", "Cancel-Me-1", false, summary, error,
            [](double p) { return p < 0.7; }) == Status::Cancelled);
        CHECK(!Exists(dir + "\\fresh"));
        RemoveTree(dir);
    }

    void ExtractBadInputs()
    {
        const std::string dir = NewDir("xbad");
        WriteFile(dir + "\\not-an-archive.txt", "just text, not an archive at all");
        ZipEngine::ExtractSummary summary;
        std::string error;
        CHECK(Extract(dir + "\\missing.zip", dir + "\\out", "", false, summary, error) == Status::FileNotFound);
        CHECK(Extract(dir + "\\not-an-archive.txt", dir + "\\out", "", false, summary, error) == Status::FileReadError);
        CHECK(Extract(dir + "\\not-an-archive.txt", "", "", false, summary, error) == Status::InvalidArgument);
        CHECK(Extract(dir + "\\not-an-archive.txt", dir + "\\not-an-archive.txt", "", false, summary, error) == Status::InvalidArgument);
        CHECK(!Exists(dir + "\\out"));
        RemoveTree(dir);
    }

    void ExtractSevenZipArchives()
    {
        // Archives made by 7-Zip itself: AES-256 and legacy ZipCrypto ZIPs with nested folders.
        const std::wstring sevenZip = SevenZipPath();
        if (sevenZip.empty())
        {
            std::printf("  (skipped: 7-Zip is not installed)\n");
            return;
        }

        const std::string dir = NewDir("x7z");
        MakeDir(dir + "\\src");
        MakeDir(dir + "\\src\\inner");
        WriteFile(dir + "\\src\\top.txt", "top level");
        WriteFile(dir + "\\src\\inner\\deep.bin", Pseudorandom(50000, 41));

        for (const wchar_t* method : { L"AES256", L"ZipCrypto" })
        {
            const std::string archive = dir + "\\by7z-" + Utf8(method) + ".zip";

            // -r recurses; running in src keeps the paths relative ("top.txt", "inner\deep.bin").
            const std::wstring cmd = L"cmd.exe /c cd /d \"" + Wide(dir) + L"\\src\" && \"" + sevenZip + L"\" a -tzip -r -mem=" + method +
                L" -pSeven-Zip-Pass \"" + Wide(archive) + L"\" * >nul";

            CHECK(Run(cmd) == 0);

            const std::string dest = dir + "\\out-" + Utf8(method);
            ZipEngine::ExtractSummary summary;
            std::string error;
            CHECK(Extract(archive, dest, "", false, summary, error) == Status::PasswordRequired);
            CHECK(Extract(archive, dest, "wrong", false, summary, error) == Status::WrongPassword);

            const Status status = Extract(archive, dest, "Seven-Zip-Pass", false, summary, error);
            if (status != Status::Success)
            {
                std::printf("  7-Zip %S archive: %s\n", method, error.c_str());
            }

            CHECK(status == Status::Success);
            CHECK(ReadFile(dest + "\\top.txt") == "top level");
            CHECK(ReadFile(dest + "\\inner\\deep.bin") == Pseudorandom(50000, 41));
        }

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
        { "ZipCryptoLegacyOption", ZipCryptoLegacyOption },
        { "TarFormatsRoundTrip", TarFormatsRoundTrip },
        { "TarExeCapabilityProbe", TarExeCapabilityProbe },
        { "TarRulesMatchZip", TarRulesMatchZip },
        { "TarCancelKeepsOldFile", TarCancelKeepsOldFile },
        { "FormatDetection", FormatDetection },
        { "ListArchiveWorks", ListArchiveWorks },
        { "ExtractRoundTripAllFormats", ExtractRoundTripAllFormats },
        { "ExtractZipPasswords", ExtractZipPasswords },
        { "ExtractRefusesUnsafeEntries", ExtractRefusesUnsafeEntries },
        { "ExtractSkipsLinks", ExtractSkipsLinks },
        { "ExtractFoldersFromOtherTools", ExtractFoldersFromOtherTools },
        { "ExtractExistingFiles", ExtractExistingFiles },
        { "ExtractCancelRollsBack", ExtractCancelRollsBack },
        { "ExtractBadInputs", ExtractBadInputs },
        { "ExtractSevenZipArchives", ExtractSevenZipArchives },
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
