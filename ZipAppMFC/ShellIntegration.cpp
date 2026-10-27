// ShellIntegration.cpp : adds and removes the app's Explorer right-click menu entries.
//
// Only keys named ZipAppMFC.* are ever written or deleted. The per-extension keys under
// SystemFileAssociations may be shared with other programs, so they are removed only when
// nothing else is left in them.

#include "pch.h"
#include "ShellIntegration.h"

#include <shlobj.h>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace
{
    const LPCWSTR kClasses = L"Software\\Classes";
    const LPCWSTR kAddKey = L"Software\\Classes\\*\\shell\\ZipAppMFC.Add";
    const LPCWSTR kExtractVerb = L"ZipAppMFC.Extract";

    // .tar.gz and friends are seen by Explorer as .gz/.bz2/.xz; a plain .gz that is not a TAR
    // is reported as "not a ZIP or TAR archive" when it is opened.
    const LPCWSTR kArchiveExtensions[] = { L".zip", L".tar", L".tgz", L".tbz2", L".tbz", L".txz", L".gz", L".bz2", L".xz" };

    CString ExePath()
    {
        wchar_t path[MAX_PATH * 4] = {};
        GetModuleFileNameW(nullptr, path, _countof(path));
        return path;
    }

    CString Command(const CString& exe, LPCWSTR verb)
    {
        CString command;
        command.Format(L"\"%s\" %s \"%%1\"", exe.GetString(), verb);
        return command;
    }

    CString ExtractKey(LPCWSTR extension)
    {
        CString key;
        key.Format(L"Software\\Classes\\SystemFileAssociations\\%s\\shell\\%s", extension, kExtractVerb);
        return key;
    }

    LSTATUS SetString(HKEY key, LPCWSTR name, const CString& value)
    {
        return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.GetString()),
            static_cast<DWORD>((value.GetLength() + 1) * sizeof(wchar_t)));
    }

    // Creates <path> with its menu text and icon, and <path>\command.
    LSTATUS WriteVerb(const CString& path, LPCWSTR text, const CString& exe, const CString& command)
    {
        HKEY key = nullptr;
        LSTATUS rc = RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
        if (rc != ERROR_SUCCESS)
        {
            return rc;
        }

        rc = SetString(key, L"MUIVerb", text);
        if (rc == ERROR_SUCCESS)
        {
            rc = SetString(key, L"Icon", L"\"" + exe + L"\",0");
        }

        RegCloseKey(key);
        if (rc != ERROR_SUCCESS)
        {
            return rc;
        }

        rc = RegCreateKeyExW(HKEY_CURRENT_USER, path + L"\\command", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
        if (rc != ERROR_SUCCESS)
        {
            return rc;
        }

        rc = SetString(key, nullptr, command);
        RegCloseKey(key);
        return rc;
    }

    bool ReadDefault(const CString& path, CString& value)
    {
        wchar_t buf[MAX_PATH * 4] = {};
        DWORD size = sizeof(buf) - sizeof(wchar_t);
        if (RegGetValueW(HKEY_CURRENT_USER, path, nullptr, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        {
            return false;
        }

        value = buf;
        return true;
    }

    bool DeleteTree(const CString& path)
    {
        const LSTATUS rc = RegDeleteTreeW(HKEY_CURRENT_USER, path);
        if (rc == ERROR_FILE_NOT_FOUND)
        {
            return true;
        }

        return rc == ERROR_SUCCESS && RegDeleteKeyW(HKEY_CURRENT_USER, path) != ERROR_ACCESS_DENIED;
    }

    // Deletes a key only if it holds no subkeys and no values (it may belong to other programs).
    void DeleteIfEmpty(const CString& path)
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        {
            return;
        }

        DWORD subkeys = 0, values = 0;
        const LSTATUS rc = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values,
            nullptr, nullptr, nullptr, nullptr);
        RegCloseKey(key);
        if (rc == ERROR_SUCCESS && subkeys == 0 && values == 0)
        {
            RegDeleteKeyW(HKEY_CURRENT_USER, path);
        }
    }

    void NotifyExplorer()
    {
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }
}

namespace shell
{
    State GetState()
    {
        CString command;
        if (!ReadDefault(CString(kAddKey) + L"\\command", command))
        {
            return State::NotRegistered;
        }

        return command.CompareNoCase(Command(ExePath(), L"/add")) == 0 ? State::Registered : State::RegisteredElsewhere;
    }

    bool Register(CString& error)
    {
        const CString exe = ExePath();

        LSTATUS rc = WriteVerb(kAddKey, L"Add to archive (ZIP / TAR)...", exe, Command(exe, L"/add"));
        for (size_t i = 0; rc == ERROR_SUCCESS && i < _countof(kArchiveExtensions); ++i)
        {
            rc = WriteVerb(ExtractKey(kArchiveExtensions[i]), L"Extract with ZIP / TAR Archive Tool...", exe,
                Command(exe, L"/extract"));
        }

        if (rc != ERROR_SUCCESS)
        {
            Unregister();
            error.Format(L"Could not write to the registry (HKEY_CURRENT_USER\\%s): error %ld.", kClasses, static_cast<long>(rc));
            return false;
        }

        NotifyExplorer();
        return true;
    }

    bool Unregister()
    {
        bool ok = DeleteTree(kAddKey);
        for (LPCWSTR extension : kArchiveExtensions)
        {
            ok = DeleteTree(ExtractKey(extension)) && ok;

            CString parent;
            parent.Format(L"Software\\Classes\\SystemFileAssociations\\%s", extension);
            DeleteIfEmpty(parent + L"\\shell");
            DeleteIfEmpty(parent);
        }

        NotifyExplorer();
        return ok;
    }
}
