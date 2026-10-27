// UiHelpers.h : small helpers shared by the Create and Extract pages.
#pragma once

#include "../ZipEngineLib/ZipEngine.h"

namespace ui
{
    // MFC uses UTF-16 (CString in a Unicode build); ZipEngine uses UTF-8. Convert only at the
    // boundary, with CP_UTF8, so any file name or password character survives.
    inline std::string ToUtf8(const CString& s)
    {
        return std::string(CW2A(s, CP_UTF8));
    }

    inline CString FromUtf8(const std::string& s)
    {
        return CString(CA2W(s.c_str(), CP_UTF8));
    }

    // Overwrites a string's characters before it is released, so a password does not linger
    // in freed heap memory.
    inline void Wipe(std::string& s)
    {
        if (!s.empty())
        {
            SecureZeroMemory(&s[0], s.size());
        }

        s.clear();
    }

    inline void Wipe(CString& s)
    {
        if (!s.IsEmpty())
        {
            SecureZeroMemory(s.GetBuffer(), s.GetLength() * sizeof(TCHAR));
        }

        s.ReleaseBuffer(0);
        s.Empty();
    }

    // The themed progress bar animates towards a new position, so a fast job would end with
    // the bar visibly short of 100%. Moving backwards is instant, so overshoot and step back.
    inline void SetProgressInstant(CProgressCtrl& bar, int pos)
    {
        int low = 0, high = 0;
        bar.GetRange(low, high);
        bar.SetRange32(low, high + 1);
        bar.SetPos(pos + 1);
        bar.SetPos(pos);
        bar.SetRange32(low, high);
        bar.SetPos(pos);
    }

    // Strips a recognised archive extension (".zip", ".tar.gz", ".tgz", ...) from a path.
    inline CString WithoutArchiveExtension(const CString& path)
    {
        static const LPCTSTR exts[] = { _T(".tar.gz"), _T(".tar.bz2"), _T(".tar.xz"), _T(".tgz"), _T(".tbz2"),
                                        _T(".tbz"), _T(".txz"), _T(".tar"), _T(".zip") };
        CString lower = path;
        lower.MakeLower();
        for (LPCTSTR ext : exts)
        {
            const int len = static_cast<int>(_tcslen(ext));
            if (lower.GetLength() > len && lower.Right(len) == ext)
            {
                return path.Left(path.GetLength() - len);
            }
        }

        return path;
    }

    inline CString FormatSize(ULONGLONG bytes)
    {
        CString s;
        if (bytes < 1024)
        {
            s.Format(_T("%llu bytes"), bytes);
        }
        else if (bytes < 1024ULL * 1024)
        {
            s.Format(_T("%.1f KB"), bytes / 1024.0);
        }
        else if (bytes < 1024ULL * 1024 * 1024)
        {
            s.Format(_T("%.1f MB"), bytes / (1024.0 * 1024));
        }
        else
        {
            s.Format(_T("%.2f GB"), bytes / (1024.0 * 1024 * 1024));
        }

        return s;
    }
}
