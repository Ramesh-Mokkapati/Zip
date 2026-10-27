// main.cpp : zipcli, the command-line front end to ZipEngineLib.
//
//   zipcli create [options] <archive> <file|pattern>...
//   zipcli list <archive>
//   zipcli extract [options] <archive>
//
// Design notes:
//  - wmain receives UTF-16 arguments, so any file name works; they are converted to UTF-8
//    for the engine. Output goes through WriteConsoleW (console) or UTF-8 (redirected), so
//    non-English names display correctly either way.
//  - Windows does not expand wildcards for programs; `*` and `?` in file arguments are
//    expanded here (files only, not recursive).
//  - Passwords given with --password are visible to other processes and saved in shell
//    history, so --password-prompt (hidden, asked twice) and --password-env are offered.
//  - Ctrl+C cancels cleanly: the engine stops and no partial archive is left behind; a
//    cancelled extraction removes the files it already wrote.
//  - extract refuses unsafe archives (entries escaping the folder, device names, ...) before
//    writing anything, and never replaces existing files without --force.
//
// Exit codes: 0 success, 1 usage error, 2 operation failed, 3 cancelled.

#include "ZipEngine.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace
{
    const int kExitOk = 0;
    const int kExitUsage = 1;
    const int kExitFailed = 2;
    const int kExitCancelled = 3;

    const wchar_t kVersion[] = L"1.2.0";

    std::atomic<bool> g_cancel(false);

    // ---- Text output ----------------------------------------------------------------

    // Writes UTF-16 text to stdout/stderr: WriteConsoleW for a console (correct Unicode
    // display), UTF-8 bytes when redirected to a file or pipe.
    void Write(DWORD stdHandle, const std::wstring& text)
    {
        HANDLE h = GetStdHandle(stdHandle);
        DWORD mode = 0, written = 0;
        if (GetConsoleMode(h, &mode))
        {
            WriteConsoleW(h, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr);
            return;
        }

        const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &utf8[0], n, nullptr, nullptr);
        WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }

    void Out(const std::wstring& text)
    {
        Write(STD_OUTPUT_HANDLE, text);
    }

    void Err(const std::wstring& text)
    {
        Write(STD_ERROR_HANDLE, text);
    }

    bool StderrIsConsole()
    {
        DWORD mode = 0;
        return GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &mode) != 0;
    }

    std::string Utf8(const std::wstring& w)
    {
        if (w.empty())
        {
            return std::string();
        }

        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
        return s;
    }

    std::wstring Wide(const std::string& s)
    {
        if (s.empty())
        {
            return std::wstring();
        }

        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
        return w;
    }

    std::wstring FormatSize(unsigned long long bytes)
    {
        wchar_t buf[64];
        if (bytes < 1024)
        {
            swprintf_s(buf, L"%llu B", bytes);
        }
        else if (bytes < 1024ULL * 1024)
        {
            swprintf_s(buf, L"%.1f KB", bytes / 1024.0);
        }
        else if (bytes < 1024ULL * 1024 * 1024)
        {
            swprintf_s(buf, L"%.1f MB", bytes / (1024.0 * 1024));
        }
        else
        {
            swprintf_s(buf, L"%.2f GB", bytes / (1024.0 * 1024 * 1024));
        }

        return buf;
    }

    void PrintUsage()
    {
        Out(L"zipcli - create and extract ZIP and TAR archives\n"
            L"\n"
            L"Usage:\n"
            L"  zipcli create [options] <archive> <file|pattern>...\n"
            L"  zipcli list <archive>\n"
            L"  zipcli extract [options] <archive>\n"
            L"  zipcli --help | --version\n"
            L"\n"
            L"The format comes from the archive's extension: .zip, .tar, .tar.gz/.tgz,\n"
            L".tar.bz2/.tbz2, .tar.xz/.txz. Wildcards (* and ?) select files in a folder.\n"
            L"Files are stored by name only; folders are not accepted.\n"
            L"\n"
            L"Options for create:\n"
            L"  -f, --format <fmt>       zip | tar | tar.gz | tar.bz2 | tar.xz (overrides the extension)\n"
            L"  -e, --encryption <m>     aes256 (default) | zipcrypto, for a password-protected ZIP\n"
            L"      --legacy             same as --encryption zipcrypto\n"
            L"  -p, --password-prompt    ask for a password (hidden, asked twice); ZIP only\n"
            L"      --password-env <VAR> read the password from environment variable VAR; ZIP only\n"
            L"      --password <text>    password on the command line (visible to other\n"
            L"                           programs and kept in shell history; prefer the above)\n"
            L"      --force              replace the archive if it already exists\n"
            L"  -q, --quiet              no progress or summary output\n"
            L"\n"
            L"With a password, ZIP entries are encrypted with AES-256 (open with zipcli extract,\n"
            L"7-Zip, WinRAR or WinZip; not Windows Explorer). Passwords may use English letters,\n"
            L"digits, spaces and punctuation.\n"
            L"--legacy uses the old ZipCrypto method instead, which Windows Explorer can open, but\n"
            L"which is weak: free tools can recover the files. Use it only when Explorer matters\n"
            L"more than secrecy.\n"
            L"TAR archives cannot be encrypted.\n"
            L"\n"
            L"Options for extract:\n"
            L"  -d, --dest <folder>      folder to extract into (created if needed); default:\n"
            L"                           a folder named after the archive, in the current folder\n"
            L"  -p, --password-prompt    ask for the password (hidden); also asked automatically\n"
            L"                           for an encrypted archive when run from a console\n"
            L"      --password-env <VAR> read the password from environment variable VAR\n"
            L"      --password <text>    password on the command line (see the note above)\n"
            L"      --force              replace files that already exist in the folder\n"
            L"  -q, --quiet              no progress or summary output\n"
            L"\n"
            L"Extraction checks the whole archive before writing: entries that would land outside\n"
            L"the folder, or names Windows cannot use safely, stop it with nothing written. Links\n"
            L"and special files in TAR archives are skipped (and reported).\n"
            L"\n"
            L"Exit codes: 0 success, 1 usage error, 2 failed, 3 cancelled (Ctrl+C).\n"
            L"\n"
            L"Examples:\n"
            L"  zipcli create -p secrets.zip report.docx budget.xlsx\n"
            L"  zipcli create logs.tar.gz C:\\Logs\\*.log\n"
            L"  zipcli create -p --legacy for-explorer.zip notes.txt\n"
            L"  zipcli list secrets.zip\n"
            L"  zipcli extract secrets.zip -d C:\\Restore\n");
    }

    int UsageError(const std::wstring& message)
    {
        Err(L"zipcli: " + message + L"\nRun 'zipcli --help' for usage.\n");
        return kExitUsage;
    }

    // ---- Input helpers --------------------------------------------------------------

    // Ctrl+C / Ctrl+Break: ask the engine to stop instead of killing the process mid-write.
    BOOL WINAPI OnConsoleCtrl(DWORD type)
    {
        if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT)
        {
            g_cancel = true;
            return TRUE;
        }

        return FALSE;
    }

    // Reads a line from the console without echoing it. Returns false if there is no console.
    bool ReadHidden(const std::wstring& prompt, std::wstring& result)
    {
        result.clear();
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        if (!GetConsoleMode(in, &mode))
        {
            return false;
        }

        Err(prompt);
        SetConsoleMode(in, (mode & ~ENABLE_ECHO_INPUT) | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
        wchar_t buf[512];
        DWORD read = 0;
        const BOOL ok = ReadConsoleW(in, buf, _countof(buf) - 1, &read, nullptr);
        SetConsoleMode(in, mode);
        Err(L"\n");
        if (!ok)
        {
            return false;
        }

        result.assign(buf, read);
        while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n'))
        {
            result.pop_back();
        }

        SecureZeroMemory(buf, sizeof(buf));
        return true;
    }

    void Wipe(std::wstring& s)
    {
        if (!s.empty())
        {
            SecureZeroMemory(&s[0], s.size() * sizeof(wchar_t));
        }

        s.clear();
    }

    void Wipe(std::string& s)
    {
        if (!s.empty())
        {
            SecureZeroMemory(&s[0], s.size());
        }

        s.clear();
    }

    // Expands * and ? in the file-name part of a path (files only). A path without
    // wildcards is returned as-is, so a missing file is reported by the engine with its name.
    bool ExpandPattern(const std::wstring& pattern, std::vector<std::wstring>& out)
    {
        if (pattern.find_first_of(L"*?") == std::wstring::npos)
        {
            out.push_back(pattern);
            return true;
        }

        const size_t slash = pattern.find_last_of(L"\\/");
        const std::wstring dir = slash == std::wstring::npos ? L"" : pattern.substr(0, slash + 1);

        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        bool any = false;
        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                out.push_back(dir + fd.cFileName);
                any = true;
            }
        } while (FindNextFileW(h, &fd));

        FindClose(h);
        return any;
    }

    // Reads the password chosen by --password / --password-env / --password-prompt (asked
    // twice when confirm is set). Returns kExitOk, or the exit code to stop with (the error
    // has been printed).
    int ReadPasswordOption(bool havePasswordArg, const std::wstring& passwordArg, const std::wstring& passwordEnv,
        bool promptPassword, bool confirm, std::wstring& password)
    {
        if (havePasswordArg)
        {
            password = passwordArg;
        }
        else if (!passwordEnv.empty())
        {
            wchar_t buf[512] = {};
            const DWORD n = GetEnvironmentVariableW(passwordEnv.c_str(), buf, _countof(buf));
            if (n == 0 || n >= _countof(buf))
            {
                return UsageError(L"environment variable " + passwordEnv + L" is not set (or is too long)");
            }

            password.assign(buf, n);
            SecureZeroMemory(buf, sizeof(buf));
        }
        else if (promptPassword)
        {
            if (!ReadHidden(L"Password: ", password))
            {
                return UsageError(L"--password-prompt needs an interactive console; use --password-env in scripts");
            }

            if (confirm)
            {
                std::wstring again;
                if (!ReadHidden(L"Confirm password: ", again))
                {
                    return UsageError(L"--password-prompt needs an interactive console; use --password-env in scripts");
                }

                const bool match = password == again;
                Wipe(again);
                if (!match)
                {
                    Wipe(password);
                    Err(L"zipcli: the passwords do not match.\n");
                    return kExitFailed;
                }
            }
        }

        return kExitOk;
    }

    // "C:\x\backup.tar.gz" -> "backup": the default folder name for extract.
    std::wstring ArchiveBaseName(const std::wstring& archive)
    {
        const size_t slash = archive.find_last_of(L"\\/:");
        std::wstring name = slash == std::wstring::npos ? archive : archive.substr(slash + 1);

        static const wchar_t* const exts[] = { L".tar.gz", L".tar.bz2", L".tar.xz", L".tgz", L".tbz2",
                                               L".tbz", L".txz", L".tar", L".zip" };
        for (const wchar_t* ext : exts)
        {
            const size_t len = wcslen(ext);
            if (name.size() > len && _wcsicmp(name.c_str() + name.size() - len, ext) == 0)
            {
                name.resize(name.size() - len);
                break;
            }
        }

        return name;
    }

    // ---- Commands ---------------------------------------------------------------------

    int CmdCreate(const std::vector<std::wstring>& args)
    {
        std::wstring formatArg, passwordArg, passwordEnv, archive, encryptionArg;
        bool promptPassword = false, force = false, quiet = false, havePasswordArg = false;
        std::vector<std::wstring> patterns;

        for (size_t i = 0; i < args.size(); ++i)
        {
            const std::wstring& a = args[i];

            // Consumes the next argument as this option's value.
            auto needValue = [&](std::wstring& target) -> bool {
                if (i + 1 >= args.size())
                {
                    return false;
                }
                target = args[++i];
                return true;
            };

            if (a == L"-f" || a == L"--format")
            {
                if (!needValue(formatArg))
                {
                    return UsageError(L"--format needs a value, e.g. --format tar.gz");
                }
            }
            else if (a == L"-e" || a == L"--encryption")
            {
                if (!needValue(encryptionArg))
                {
                    return UsageError(L"--encryption needs a value: aes256 or zipcrypto");
                }
            }
            else if (a == L"--legacy")
            {
                encryptionArg = L"zipcrypto";
            }
            else if (a == L"-p" || a == L"--password-prompt")
            {
                promptPassword = true;
            }
            else if (a == L"--password-env")
            {
                if (!needValue(passwordEnv))
                {
                    return UsageError(L"--password-env needs a variable name");
                }
            }
            else if (a == L"--password")
            {
                if (!needValue(passwordArg))
                {
                    return UsageError(L"--password needs a value");
                }

                havePasswordArg = true;
            }
            else if (a == L"--force")
            {
                force = true;
            }
            else if (a == L"-q" || a == L"--quiet")
            {
                quiet = true;
            }
            else if (a.size() > 1 && a[0] == L'-' && a != L"-")
            {
                return UsageError(L"unknown option " + a);
            }
            else if (archive.empty())
            {
                archive = a;
            }
            else
            {
                patterns.push_back(a);
            }
        }

        if (archive.empty())
        {
            return UsageError(L"no archive name given");
        }

        if (patterns.empty())
        {
            return UsageError(L"no files given");
        }

        if ((promptPassword ? 1 : 0) + (havePasswordArg ? 1 : 0) + (passwordEnv.empty() ? 0 : 1) > 1)
        {
            return UsageError(L"use only one of --password-prompt, --password-env and --password");
        }

        // Format: explicit option, else the archive's extension.
        ZipEngine::ArchiveFormat format;
        if (!formatArg.empty())
        {
            if (!ZipEngine::FormatFromName(Utf8(formatArg), format))
            {
                return UsageError(L"unknown format '" + formatArg + L"' (use zip, tar, tar.gz, tar.bz2 or tar.xz)");
            }
        }
        else if (!ZipEngine::FormatFromPath(Utf8(archive), format))
        {
            return UsageError(L"cannot tell the format from '" + archive +
                L"'. Use a .zip, .tar, .tar.gz, .tar.bz2 or .tar.xz name, or --format.");
        }

        const bool wantsPassword = promptPassword || havePasswordArg || !passwordEnv.empty();
        if (wantsPassword && !ZipEngine::FormatSupportsEncryption(format))
        {
            return UsageError(Wide(ZipEngine::FormatName(format)) + L" archives cannot be encrypted; use a .zip archive for a password.");
        }

        ZipEngine::ZipEncryption encryption = ZipEngine::ZipEncryption::Aes256;
        if (!encryptionArg.empty())
        {
            if (_wcsicmp(encryptionArg.c_str(), L"zipcrypto") == 0 || _wcsicmp(encryptionArg.c_str(), L"legacy") == 0)
            {
                encryption = ZipEngine::ZipEncryption::ZipCrypto;
            }
            else if (_wcsicmp(encryptionArg.c_str(), L"aes256") != 0 && _wcsicmp(encryptionArg.c_str(), L"aes") != 0)
            {
                return UsageError(L"unknown encryption '" + encryptionArg + L"' (use aes256 or zipcrypto)");
            }

            if (!wantsPassword)
            {
                return UsageError(L"--encryption and --legacy need a password (-p, --password-env or --password)");
            }
        }

        const bool legacy = encryption == ZipEngine::ZipEncryption::ZipCrypto;

        // Files: expand wildcards.
        std::vector<std::wstring> files;
        for (const std::wstring& p : patterns)
        {
            if (!ExpandPattern(p, files))
            {
                Err(L"zipcli: no files match " + p + L"\n");
                return kExitFailed;
            }
        }

        // Existing archive: only replaced with --force (scripts should not overwrite by accident).
        if (!force && GetFileAttributesW(archive.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            Err(L"zipcli: " + archive + L" already exists. Use --force to replace it.\n");
            return kExitFailed;
        }

        // Password.
        std::wstring password;
        const int passwordResult = ReadPasswordOption(havePasswordArg, passwordArg, passwordEnv, promptPassword, true, password);
        if (passwordResult != kExitOk)
        {
            return passwordResult;
        }

        if (wantsPassword && password.empty())
        {
            return UsageError(L"the password is empty");
        }

        // Run.
        std::vector<std::string> utf8Files;
        for (const std::wstring& f : files)
        {
            utf8Files.push_back(Utf8(f));
        }

        std::string utf8Password = Utf8(password);
        Wipe(password);

        SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
        const bool showProgress = !quiet && StderrIsConsole();
        int lastPercent = -1;
        std::string error;
        const ZipEngine::Status status = ZipEngine::CreateArchive(
            format, Utf8(archive), utf8Files, utf8Password, encryption,
            [&](double progress) -> bool {
            const int percent = static_cast<int>(progress * 100.0);
            if (showProgress && percent != lastPercent)
            {
                lastPercent = percent;
                wchar_t line[64];
                swprintf_s(line, L"\rCreating %s... %3d%%", Wide(ZipEngine::FormatName(format)).c_str(), percent);
                Err(line);
            }
            return !g_cancel.load();
        },
            error);
        Wipe(utf8Password);
        if (showProgress)
        {
            Err(L"\r                                        \r");
        }

        if (status == ZipEngine::Status::Cancelled)
        {
            Err(L"zipcli: cancelled; no archive was written.\n");
            return kExitCancelled;
        }

        if (status != ZipEngine::Status::Success)
        {
            Err(L"zipcli: " + Wide(ZipEngine::StatusToString(status)) + L": " + Wide(error) + L"\n");
            return kExitFailed;
        }

        if (!quiet)
        {
            WIN32_FILE_ATTRIBUTE_DATA info = {};
            unsigned long long size = 0;
            if (GetFileAttributesExW(archive.c_str(), GetFileExInfoStandard, &info))
            {
                size = (static_cast<unsigned long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
            }

            wchar_t count[32];
            swprintf_s(count, L"%u", static_cast<unsigned>(files.size()));
            Out(L"Created " + archive + L" (" + Wide(ZipEngine::FormatName(format)) + L", " + FormatSize(size) + L", " +
                count + L" file(s)" + (!wantsPassword ? L"" : legacy ? L", ZipCrypto encrypted" : L", AES-256 encrypted") + L")\n");
            if (wantsPassword && legacy)
            {
                Err(L"zipcli: note: ZipCrypto is weak encryption that free tools can break; it was used because\n"
                    L"        --legacy / --encryption zipcrypto was given (for Windows Explorer).\n");
            }
        }

        return kExitOk;
    }

    int CmdList(const std::vector<std::wstring>& args)
    {
        if (args.size() != 1)
        {
            return UsageError(L"list takes exactly one archive name");
        }

        std::vector<ZipEngine::ArchiveEntryInfo> entries;
        std::string error;
        const ZipEngine::Status status = ZipEngine::ListArchive(Utf8(args[0]), entries, error);
        if (status != ZipEngine::Status::Success)
        {
            Err(L"zipcli: " + Wide(error) + L"\n");
            return kExitFailed;
        }

        unsigned long long total = 0;
        bool anyEncrypted = false;
        for (const auto& e : entries)
        {
            wchar_t size[32];
            swprintf_s(size, L"%14llu", e.size);
            Out(std::wstring(size) + (e.encrypted ? L"  *" : L"   ") + L"  " + Wide(e.name) + (e.isDirectory ? L"/" : L"") + L"\n");
            total += e.size;
            anyEncrypted = anyEncrypted || e.encrypted;
        }

        wchar_t summary[128];
        swprintf_s(summary, L"%u entries, %llu bytes", static_cast<unsigned>(entries.size()), total);
        Out(std::wstring(summary) + (anyEncrypted ? L"  (* = encrypted)" : L"") + L"\n");
        return kExitOk;
    }

    int CmdExtract(const std::vector<std::wstring>& args)
    {
        std::wstring dest, passwordArg, passwordEnv, archive;
        bool promptPassword = false, force = false, quiet = false, havePasswordArg = false;

        for (size_t i = 0; i < args.size(); ++i)
        {
            const std::wstring& a = args[i];

            // Consumes the next argument as this option's value.
            auto needValue = [&](std::wstring& target) -> bool {
                if (i + 1 >= args.size())
                {
                    return false;
                }
                target = args[++i];
                return true;
            };

            if (a == L"-d" || a == L"--dest")
            {
                if (!needValue(dest) || dest.empty())
                {
                    return UsageError(L"--dest needs a folder");
                }
            }
            else if (a == L"-p" || a == L"--password-prompt")
            {
                promptPassword = true;
            }
            else if (a == L"--password-env")
            {
                if (!needValue(passwordEnv))
                {
                    return UsageError(L"--password-env needs a variable name");
                }
            }
            else if (a == L"--password")
            {
                if (!needValue(passwordArg))
                {
                    return UsageError(L"--password needs a value");
                }

                havePasswordArg = true;
            }
            else if (a == L"--force")
            {
                force = true;
            }
            else if (a == L"-q" || a == L"--quiet")
            {
                quiet = true;
            }
            else if (a.size() > 1 && a[0] == L'-' && a != L"-")
            {
                return UsageError(L"unknown option " + a);
            }
            else if (archive.empty())
            {
                archive = a;
            }
            else
            {
                return UsageError(L"extract takes one archive (use -d to choose the folder)");
            }
        }

        if (archive.empty())
        {
            return UsageError(L"no archive name given");
        }

        if ((promptPassword ? 1 : 0) + (havePasswordArg ? 1 : 0) + (passwordEnv.empty() ? 0 : 1) > 1)
        {
            return UsageError(L"use only one of --password-prompt, --password-env and --password");
        }

        const DWORD attrs = GetFileAttributesW(archive.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY))
        {
            Err(L"zipcli: " + archive + L" not found.\n");
            return kExitFailed;
        }

        // Look inside first, to know whether a password is needed.
        std::vector<ZipEngine::ArchiveEntryInfo> entries;
        std::string error;
        if (ZipEngine::ListArchive(Utf8(archive), entries, error) != ZipEngine::Status::Success)
        {
            Err(L"zipcli: " + Wide(error) + L"\n");
            return kExitFailed;
        }

        bool anyEncrypted = false;
        for (const auto& e : entries)
        {
            anyEncrypted = anyEncrypted || e.encrypted;
        }

        std::wstring password;
        const int passwordResult = ReadPasswordOption(havePasswordArg, passwordArg, passwordEnv, promptPassword, false, password);
        if (passwordResult != kExitOk)
        {
            return passwordResult;
        }

        // Encrypted and no password option given: ask, if there is a console to ask on.
        if (anyEncrypted && !havePasswordArg && passwordEnv.empty() && !promptPassword)
        {
            if (!ReadHidden(L"The archive is encrypted. Password: ", password))
            {
                Err(L"zipcli: " + archive + L" is encrypted; give the password with --password-env or --password.\n");
                return kExitFailed;
            }
        }

        if (dest.empty())
        {
            dest = ArchiveBaseName(archive);
            if (dest.empty())
            {
                return UsageError(L"cannot name a folder after '" + archive + L"'; use -d");
            }
        }

        std::string utf8Password = Utf8(password);
        Wipe(password);

        SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
        const bool showProgress = !quiet && StderrIsConsole();
        int lastPercent = -1;
        ZipEngine::ExtractSummary summary;
        const ZipEngine::Status status = ZipEngine::ExtractArchive(
            Utf8(archive), Utf8(dest), utf8Password, force,
            [&](double progress) -> bool {
            const int percent = static_cast<int>(progress * 100.0);
            if (showProgress && percent != lastPercent)
            {
                lastPercent = percent;
                wchar_t line[64];
                swprintf_s(line, L"\rExtracting... %3d%%", percent);
                Err(line);
            }
            return !g_cancel.load();
        },
            summary, error);

        Wipe(utf8Password);
        if (showProgress)
        {
            Err(L"\r                                        \r");
        }

        switch (status)
        {

        case ZipEngine::Status::Success:
            break;

        case ZipEngine::Status::Cancelled:
            Err(L"zipcli: cancelled; the files extracted so far were removed.\n");
            return kExitCancelled;

        case ZipEngine::Status::FileExists:
            Err(L"zipcli: " + Wide(error) + L"\nNothing was extracted. Use --force to replace existing files.\n");
            return kExitFailed;

        case ZipEngine::Status::UnsafeEntry:
            Err(L"zipcli: refused: " + Wide(error) + L"\nNothing was extracted.\n");
            return kExitFailed;

        default:
            Err(L"zipcli: " + Wide(ZipEngine::StatusToString(status)) + L": " + Wide(error) + L"\n");
            return kExitFailed;
        }

        for (const std::string& skipped : summary.skipped)
        {
            Err(L"zipcli: skipped " + Wide(skipped) + L"\n");
        }

        if (!quiet)
        {
            wchar_t count[32];
            swprintf_s(count, L"%u", summary.files);
            Out(L"Extracted " + std::wstring(count) + L" file(s), " + FormatSize(summary.bytes) + L", to " + dest + L"\n");
        }

        return kExitOk;
    }
}

int wmain(int argc, wchar_t** argv)
{
    std::vector<std::wstring> args(argv + 1, argv + argc);
    if (args.empty())
    {
        PrintUsage();
        return kExitUsage;
    }

    const std::wstring command = args[0];
    const std::vector<std::wstring> rest(args.begin() + 1, args.end());

    if (command == L"-h" || command == L"--help" || command == L"/?" || command == L"help")
    {
        PrintUsage();
        return kExitOk;
    }

    if (command == L"--version")
    {
        Out(std::wstring(L"zipcli ") + kVersion + L"\n");
        return kExitOk;
    }

    if (command == L"create" || command == L"c")
    {
        return CmdCreate(rest);
    }

    if (command == L"list" || command == L"l")
    {
        return CmdList(rest);
    }

    if (command == L"extract" || command == L"x")
    {
        return CmdExtract(rest);
    }

    return UsageError(L"unknown command '" + command + L"' (use create, list or extract)");
}
