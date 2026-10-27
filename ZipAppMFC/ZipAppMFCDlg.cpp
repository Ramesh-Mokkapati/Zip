// ZipAppMFCDlg.cpp : main dialog of the ZIP creation tool.
//
// Threading: "Create Zip" copies everything the job needs into a ZipJob and starts
// ZipWorker on a background thread. The worker never touches MFC objects; it only posts
// WM_ZIP_PROGRESS (progress in tenths of a percent) and finally WM_ZIP_DONE (with a
// heap-allocated ZipResult that the UI thread deletes).
//
// Strings: MFC uses UTF-16 (CString in a Unicode build); ZipEngine uses UTF-8. Conversion
// happens only at the boundary, with CW2A/CA2W and CP_UTF8, so any file name or password
// character survives (the runbook's CT2A would use the ANSI code page and lose them).

#include "pch.h"
#include "ZipAppMFC.h"
#include "ZipAppMFCDlg.h"
#include "../ZipEngineLib/ZipEngine.h"

#include <shellapi.h>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace
{
    const UINT WM_ZIP_PROGRESS = WM_APP + 1;   // wParam = progress 0..1000
    const UINT WM_ZIP_DONE = WM_APP + 2;   // lParam = ZipResult* (receiver deletes)

    // Everything the worker needs, copied so the UI can keep changing while it runs.
    struct ZipJob
    {
        HWND                     hwnd;
        std::atomic<bool>* cancel;
        ZipEngine::ArchiveFormat format;
        std::string              output;     // UTF-8
        std::vector<std::string> files;      // UTF-8
        std::string              password;   // UTF-8, wiped when the job ends
    };

    struct ZipResult
    {
        ZipEngine::Status status;
        std::string       message;   // UTF-8
    };

    std::string ToUtf8(const CString& s)
    {
        return std::string(CW2A(s, CP_UTF8));
    }

    CString FromUtf8(const std::string& s)
    {
        return CString(CA2W(s.c_str(), CP_UTF8));
    }

    // Overwrites a string's characters before it is released, so the password does not
    // linger in freed heap memory.
    void Wipe(std::string& s)
    {
        if (!s.empty())
        {
            SecureZeroMemory(&s[0], s.size());
        }

        s.clear();
    }

    void Wipe(CString& s)
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
    void SetProgressInstant(CProgressCtrl& bar, int pos)
    {
        int low = 0, high = 0;
        bar.GetRange(low, high);
        bar.SetRange32(low, high + 1);
        bar.SetPos(pos + 1);
        bar.SetPos(pos);
        bar.SetRange32(low, high);
        bar.SetPos(pos);
    }

    // Combo box entries, in display order.
    struct FormatChoice
    {
        ZipEngine::ArchiveFormat format;
        LPCTSTR                  label;
    };

    const FormatChoice kFormats[] = {
        { ZipEngine::ArchiveFormat::Zip,    _T("ZIP (.zip) - can be password-protected") },
        { ZipEngine::ArchiveFormat::TarGz,  _T("TAR.GZ (.tar.gz) - gzip, widely used") },
        { ZipEngine::ArchiveFormat::TarXz,  _T("TAR.XZ (.tar.xz) - xz, smallest") },
        { ZipEngine::ArchiveFormat::TarBz2, _T("TAR.BZ2 (.tar.bz2) - bzip2") },
        { ZipEngine::ArchiveFormat::Tar,    _T("TAR (.tar) - not compressed") },
    };

    // Strips a recognised archive extension (".zip", ".tar.gz", ".tgz", ...) from a path.
    CString WithoutArchiveExtension(const CString& path)
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

    CString FormatSize(ULONGLONG bytes)
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

BEGIN_MESSAGE_MAP(CZipAppMFCDlg, CDialogEx)
    ON_BN_CLICKED(IDC_BTN_BROWSE, &CZipAppMFCDlg::OnBnClickedBtnBrowse)
    ON_BN_CLICKED(IDC_BTN_REMOVE, &CZipAppMFCDlg::OnBnClickedBtnRemove)
    ON_BN_CLICKED(IDC_BTN_CLEAR, &CZipAppMFCDlg::OnBnClickedBtnClear)
    ON_BN_CLICKED(IDC_BTN_OUTPUT, &CZipAppMFCDlg::OnBnClickedBtnOutput)
    ON_BN_CLICKED(IDC_BTN_ZIP, &CZipAppMFCDlg::OnBnClickedBtnZip)
    ON_BN_CLICKED(IDC_BTN_STOP, &CZipAppMFCDlg::OnBnClickedBtnStop)
    ON_LBN_SELCHANGE(IDC_LIST_FILES, &CZipAppMFCDlg::OnLbnSelchangeListFiles)
    ON_CBN_SELCHANGE(IDC_COMBO_FORMAT, &CZipAppMFCDlg::OnCbnSelchangeComboFormat)
    ON_WM_DROPFILES()
    ON_WM_DESTROY()
    ON_MESSAGE(WM_ZIP_PROGRESS, &CZipAppMFCDlg::OnZipProgress)
    ON_MESSAGE(WM_ZIP_DONE, &CZipAppMFCDlg::OnZipDone)
END_MESSAGE_MAP()

CZipAppMFCDlg::CZipAppMFCDlg(CWnd* pParent)
    : CDialogEx(IDD_ZIPAPPMFC_DIALOG, pParent)
{
    m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

void CZipAppMFCDlg::DoDataExchange(CDataExchange* pDX)
{
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_LIST_FILES, m_listFiles);
    DDX_Control(pDX, IDC_COMBO_FORMAT, m_comboFormat);
    DDX_Control(pDX, IDC_PROGRESS, m_progress);
}

BOOL CZipAppMFCDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    SetIcon(m_hIcon, TRUE);
    SetIcon(m_hIcon, FALSE);

    m_progress.SetRange32(0, 1000);

    // Accept files dragged from Explorer onto the window (also set via WS_EX_ACCEPTFILES).
    DragAcceptFiles(TRUE);

    // Archive formats; the item data holds the ArchiveFormat value.
    for (const FormatChoice& choice : kFormats)
    {
        const int item = m_comboFormat.AddString(choice.label);
        m_comboFormat.SetItemData(item, static_cast<DWORD_PTR>(choice.format));
    }

    SelectFormat(ZipEngine::ArchiveFormat::Zip);

    SetDlgItemText(IDC_STATUS, _T("Add files with \"Add Files...\" or drag them onto this window."));
    UpdateControls();
    return TRUE;
}

// ---------------------------------------------------------------------------
// File list

int CZipAppMFCDlg::AddFiles(const std::vector<CString>& paths)
{
    int added = 0, skippedFolders = 0;
    for (const CString& path : paths)
    {
        const DWORD attrs = GetFileAttributes(path);
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
        {
            ++skippedFolders;   // entries are stored without folders, so folders are not supported
            continue;
        }

        // The same file twice would make a duplicate entry name; just ignore repeats.
        if (m_listFiles.FindStringExact(-1, path) != LB_ERR)
        {
            continue;
        }

        m_listFiles.AddString(path);
        ++added;
    }

    // A horizontal scrollbar for long paths needs the widest item's pixel width.
    CClientDC dc(&m_listFiles);
    CFont* pOld = dc.SelectObject(m_listFiles.GetFont());
    int widest = 0;
    for (int i = 0; i < m_listFiles.GetCount(); ++i)
    {
        CString text;
        m_listFiles.GetText(i, text);
        widest = max(widest, static_cast<int>(dc.GetTextExtent(text).cx));
    }

    dc.SelectObject(pOld);
    m_listFiles.SetHorizontalExtent(widest + 8);

    CString status;
    status.Format(_T("%d file(s) in the list."), m_listFiles.GetCount());
    if (skippedFolders)
    {
        status.AppendFormat(_T("  %d folder(s) skipped: add the files inside them instead."), skippedFolders);
    }

    SetDlgItemText(IDC_STATUS, status);

    SuggestOutputPath();
    UpdateControls();
    return added;
}

// Proposes "<folder of first file>\<first file name><ext>" until the user picks an output.
void CZipAppMFCDlg::SuggestOutputPath()
{
    CString output;
    GetDlgItemText(IDC_EDIT_OUTPUT, output);
    if (!output.IsEmpty() || m_listFiles.GetCount() == 0)
    {
        return;
    }

    CString first;
    m_listFiles.GetText(0, first);
    const int dot = first.ReverseFind(_T('.'));
    const int slash = first.ReverseFind(_T('\\'));
    CString base = (dot > slash) ? first.Left(dot) : first;
    if (m_listFiles.GetCount() > 1)
    {
        base = first.Left(slash + 1) + _T("Archive");
    }

    SetDlgItemText(IDC_EDIT_OUTPUT, base + CString(ZipEngine::FormatExtension(CurrentFormat())));
}

ZipEngine::ArchiveFormat CZipAppMFCDlg::CurrentFormat() const
{
    const int sel = m_comboFormat.GetCurSel();
    return sel == CB_ERR ? ZipEngine::ArchiveFormat::Zip
        : static_cast<ZipEngine::ArchiveFormat>(m_comboFormat.GetItemData(sel));
}

void CZipAppMFCDlg::SelectFormat(ZipEngine::ArchiveFormat format)
{
    for (int i = 0; i < m_comboFormat.GetCount(); ++i)
    {
        if (static_cast<ZipEngine::ArchiveFormat>(m_comboFormat.GetItemData(i)) == format)
        {
            m_comboFormat.SetCurSel(i);
        }
    }

    UpdateControls();
}

void CZipAppMFCDlg::ApplyFormatExtension()
{
    CString output;
    GetDlgItemText(IDC_EDIT_OUTPUT, output);
    output.Trim();
    if (output.IsEmpty())
    {
        return;
    }

    SetDlgItemText(IDC_EDIT_OUTPUT, WithoutArchiveExtension(output) + CString(ZipEngine::FormatExtension(CurrentFormat())));
}

void CZipAppMFCDlg::OnCbnSelchangeComboFormat()
{
    ApplyFormatExtension();
    UpdateControls();
}

void CZipAppMFCDlg::OnBnClickedBtnBrowse()
{
    // Large buffer: multi-select returns all names in one string.
    CFileDialog dlg(TRUE, nullptr, nullptr, OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
        _T("All files (*.*)|*.*||"), this);
    std::vector<TCHAR> buffer(64 * 1024, 0);
    dlg.m_ofn.lpstrFile = buffer.data();
    dlg.m_ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    if (dlg.DoModal() != IDOK)
    {
        return;
    }

    std::vector<CString> paths;
    POSITION pos = dlg.GetStartPosition();
    while (pos)
    {
        paths.push_back(dlg.GetNextPathName(pos));
    }

    AddFiles(paths);
}

void CZipAppMFCDlg::OnDropFiles(HDROP hDropInfo)
{
    std::vector<CString> paths;
    const UINT count = DragQueryFile(hDropInfo, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i)
    {
        const UINT len = DragQueryFile(hDropInfo, i, nullptr, 0);
        CString path;
        DragQueryFile(hDropInfo, i, path.GetBuffer(len + 1), len + 1);
        path.ReleaseBuffer();
        paths.push_back(path);
    }

    DragFinish(hDropInfo);

    if (m_pWorker == nullptr)   // ignore drops while an archive is being written
    {
        AddFiles(paths);
    }
}

void CZipAppMFCDlg::OnBnClickedBtnRemove()
{
    // Delete from the end so earlier indexes stay valid.
    const int count = m_listFiles.GetSelCount();
    std::vector<int> selected(static_cast<size_t>(max(count, 0)));
    if (count > 0)
    {
        m_listFiles.GetSelItems(count, selected.data());
    }

    for (auto it = selected.rbegin(); it != selected.rend(); ++it)
    {
        m_listFiles.DeleteString(*it);
    }

    UpdateControls();
}

void CZipAppMFCDlg::OnBnClickedBtnClear()
{
    m_listFiles.ResetContent();
    m_listFiles.SetHorizontalExtent(0);
    UpdateControls();
}

void CZipAppMFCDlg::OnLbnSelchangeListFiles()
{
    UpdateControls();
}

void CZipAppMFCDlg::OnBnClickedBtnOutput()
{
    CString current;
    GetDlgItemText(IDC_EDIT_OUTPUT, current);

    // Filter and default extension for the chosen format, e.g. "TAR.GZ archives (*.tar.gz)|*.tar.gz;*.tgz||".
    const ZipEngine::ArchiveFormat format = CurrentFormat();
    const CString ext(ZipEngine::FormatExtension(format));   // ".tar.gz"
    CString patterns = _T("*") + ext;

    if (format == ZipEngine::ArchiveFormat::TarGz)
    {
        patterns += _T(";*.tgz");
    }

    if (format == ZipEngine::ArchiveFormat::TarBz2)
    {
        patterns += _T(";*.tbz2");
    }

    if (format == ZipEngine::ArchiveFormat::TarXz)
    {
        patterns += _T(";*.txz");
    }

    const CString filter = CString(ZipEngine::FormatName(format)) + _T(" archives (") + patterns + _T(")|") + patterns + _T("||");

    CFileDialog dlg(FALSE, ext.Mid(1), current, OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY, filter, this);
    if (dlg.DoModal() == IDOK)
    {
        SetDlgItemText(IDC_EDIT_OUTPUT, dlg.GetPathName());
    }
}

// Enables buttons according to the current state (idle / running, selection, list contents).
void CZipAppMFCDlg::UpdateControls()
{
    const bool running = m_pWorker != nullptr;
    const bool hasFiles = m_listFiles.GetCount() > 0;
    const bool canEncrypt = ZipEngine::FormatSupportsEncryption(CurrentFormat());
    GetDlgItem(IDC_BTN_BROWSE)->EnableWindow(!running);
    GetDlgItem(IDC_BTN_REMOVE)->EnableWindow(!running && m_listFiles.GetSelCount() > 0);
    GetDlgItem(IDC_BTN_CLEAR)->EnableWindow(!running && hasFiles);
    GetDlgItem(IDC_COMBO_FORMAT)->EnableWindow(!running);
    GetDlgItem(IDC_BTN_OUTPUT)->EnableWindow(!running);
    GetDlgItem(IDC_EDIT_OUTPUT)->EnableWindow(!running);

    // Passwords only apply to ZIP; TAR has no encryption.
    GetDlgItem(IDC_EDIT_PASS)->EnableWindow(!running && canEncrypt);
    GetDlgItem(IDC_EDIT_PASS_CONFIRM)->EnableWindow(!running && canEncrypt);
    SetDlgItemText(IDC_ENCRYPTION_NOTE, canEncrypt
        ? _T("Encrypted with AES-256. Use English letters, digits and punctuation in the password. Open the archive with ")
        _T("7-Zip, WinRAR or WinZip; Windows Explorer cannot open AES-encrypted ZIP files.")
        : _T("TAR archives cannot be password-protected. Choose ZIP if the files must be encrypted. ")
        _T("Open with 7-Zip, WinRAR, or tar (built into Windows 10 and later)."));

    GetDlgItem(IDC_BTN_ZIP)->EnableWindow(!running && hasFiles);
    GetDlgItem(IDC_BTN_STOP)->EnableWindow(running);
    m_listFiles.EnableWindow(!running);
}

// ---------------------------------------------------------------------------
// Creating the archive

// Handler for "Create Archive": validates the form, then starts the worker thread.
void CZipAppMFCDlg::OnBnClickedBtnZip()
{
    if (m_pWorker)
    {
        return;
    }

    UpdateData(TRUE);

    CString output, password, confirm;
    GetDlgItemText(IDC_EDIT_OUTPUT, output);
    output.Trim();

    if (m_listFiles.GetCount() == 0)
    {
        AfxMessageBox(_T("Add at least one file first."), MB_ICONINFORMATION);
        return;
    }

    if (output.IsEmpty())
    {
        AfxMessageBox(_T("Choose where to save the archive."), MB_ICONINFORMATION);
        GetDlgItem(IDC_EDIT_OUTPUT)->SetFocus();
        return;
    }

    // A file name like "backup.tar.gz" with ZIP selected is almost certainly a slip: offer
    // to switch to the format the name implies, so the extension tells the truth.
    ZipEngine::ArchiveFormat format = CurrentFormat();
    ZipEngine::ArchiveFormat named;
    if (ZipEngine::FormatFromPath(ToUtf8(output), named) && named != format)
    {
        CString question;
        question.Format(_T("The file name ends in %s, but the selected format is %s.\n\nCreate a %s archive instead?\n\n")
            _T("(No keeps %s and changes the extension to %s.)"),
            CString(ZipEngine::FormatExtension(named)).GetString(), CString(ZipEngine::FormatName(format)).GetString(),
            CString(ZipEngine::FormatName(named)).GetString(), CString(ZipEngine::FormatName(format)).GetString(),
            CString(ZipEngine::FormatExtension(format)).GetString());

        const int answer = AfxMessageBox(question, MB_YESNOCANCEL | MB_ICONQUESTION);
        if (answer == IDCANCEL)
        {
            return;
        }

        if (answer == IDYES)
        {
            SelectFormat(named);
            format = named;
        }
        else
        {
            ApplyFormatExtension();
            GetDlgItemText(IDC_EDIT_OUTPUT, output);
        }
    }

    const bool canEncrypt = ZipEngine::FormatSupportsEncryption(format);
    if (canEncrypt)
    {
        GetDlgItemText(IDC_EDIT_PASS, password);
        GetDlgItemText(IDC_EDIT_PASS_CONFIRM, confirm);
    }

    // A typo in a password nobody can see would make the archive impossible to open.
    if (password != confirm)
    {
        Wipe(password);
        Wipe(confirm);
        AfxMessageBox(_T("The passwords do not match. Type the same password in both boxes."), MB_ICONWARNING);
        SetDlgItemText(IDC_EDIT_PASS_CONFIRM, _T(""));
        GetDlgItem(IDC_EDIT_PASS_CONFIRM)->SetFocus();
        return;
    }

    Wipe(confirm);

    // Same rule as the engine: only printable ASCII passwords open reliably in 7-Zip/WinRAR/WinZip
    // on every computer (ZIP has no standard encoding for other characters).
    for (int i = 0; i < password.GetLength(); ++i)
    {
        if (password[i] < 0x20 || password[i] > 0x7E)
        {
            Wipe(password);
            AfxMessageBox(_T("Please use only English letters, digits, spaces and punctuation in the password.\n\n")
                _T("ZIP files have no standard way to store other characters, so 7-Zip, WinRAR or WinZip ")
                _T("on another computer might not accept the password."),
                MB_ICONWARNING);

            SetDlgItemText(IDC_EDIT_PASS, _T(""));
            SetDlgItemText(IDC_EDIT_PASS_CONFIRM, _T(""));
            GetDlgItem(IDC_EDIT_PASS)->SetFocus();
            return;
        }
    }

    if (canEncrypt && password.IsEmpty() &&
        AfxMessageBox(_T("No password was entered, so the ZIP file will NOT be encrypted.\n\nCreate it anyway?"),
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
    {
        return;
    }

    if (GetFileAttributes(output) != INVALID_FILE_ATTRIBUTES &&
        AfxMessageBox(_T("This file already exists:\n") + output + _T("\n\nReplace it?"),
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
    {
        Wipe(password);
        return;
    }

    // Copy the job; the engine works in UTF-8.
    ZipJob* job = new ZipJob;
    job->hwnd = GetSafeHwnd();
    job->cancel = &m_cancel;
    job->format = format;
    job->output = ToUtf8(output);
    for (int i = 0; i < m_listFiles.GetCount(); ++i)
    {
        CString path;
        m_listFiles.GetText(i, path);
        job->files.push_back(ToUtf8(path));
    }

    job->password = ToUtf8(password);
    Wipe(password);

    m_runOutput = output;
    m_runFileCount = job->files.size();
    m_runEncrypted = !job->password.empty();
    m_runFormat = format;
    m_cancel = false;

    m_pWorker = AfxBeginThread(ZipWorker, job, THREAD_PRIORITY_BELOW_NORMAL, 0, CREATE_SUSPENDED);
    if (!m_pWorker)
    {
        Wipe(job->password);
        delete job;
        AfxMessageBox(_T("Could not start the background thread."), MB_ICONERROR);
        return;
    }

    m_pWorker->m_bAutoDelete = FALSE;   // we wait on and delete it in OnZipDone / OnDestroy
    m_pWorker->ResumeThread();

    m_progress.SetPos(0);
    SetDlgItemText(IDC_STATUS, _T("Compressing..."));
    UpdateControls();
}

// Runs on the worker thread: everything it needs is in the ZipJob; it reports back only
// through PostMessage.
UINT __cdecl CZipAppMFCDlg::ZipWorker(LPVOID pParam)
{
    ZipJob* job = static_cast<ZipJob*>(pParam);
    int lastPermille = -1;

    // One call for every format: ZIP goes to ZipCompressor, TAR formats to TarCompressor.
    std::string errorMessage;
    const ZipEngine::Status status = ZipEngine::CreateArchive(
        job->format, job->output, job->files, job->password,
        [job, &lastPermille](double progress) -> bool
    {
        // Post only when the displayed value changes, to avoid flooding the queue.
        const int permille = static_cast<int>(progress * 1000.0);
        if (permille != lastPermille)
        {
            lastPermille = permille;
            ::PostMessage(job->hwnd, WM_ZIP_PROGRESS, static_cast<WPARAM>(permille), 0);
        }
        return !job->cancel->load();   // false = cancel
    },
        errorMessage);

    ZipResult* result = new ZipResult{ status, errorMessage };
    const HWND hwnd = job->hwnd;
    Wipe(job->password);
    delete job;

    if (!::PostMessage(hwnd, WM_ZIP_DONE, 0, reinterpret_cast<LPARAM>(result)))
    {
        delete result;   // window already gone
    }

    return 0;
}

LRESULT CZipAppMFCDlg::OnZipProgress(WPARAM wParam, LPARAM)
{
    m_progress.SetPos(static_cast<int>(wParam));
    CString text;
    text.Format(_T("Compressing%s... %d%%"), m_runEncrypted ? _T(" and encrypting") : _T(""), static_cast<int>(wParam / 10));
    SetDlgItemText(IDC_STATUS, text);
    return 0;
}

LRESULT CZipAppMFCDlg::OnZipDone(WPARAM, LPARAM lParam)
{
    std::unique_ptr<ZipResult> result(reinterpret_cast<ZipResult*>(lParam));

    if (m_pWorker)
    {
        WaitForSingleObject(m_pWorker->m_hThread, INFINITE);
        delete m_pWorker;
        m_pWorker = nullptr;
    }

    UpdateControls();

    if (result->status == ZipEngine::Status::Success)
    {
        SetProgressInstant(m_progress, 1000);

        WIN32_FILE_ATTRIBUTE_DATA info = {};
        ULONGLONG size = 0;
        if (GetFileAttributesEx(m_runOutput, GetFileExInfoStandard, &info))
        {
            size = (static_cast<ULONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        }

        // Just the file name here (a long path would push the details off the line);
        // the message box below shows the full path.
        const CString fileName = m_runOutput.Mid(m_runOutput.ReverseFind(_T('\\')) + 1);
        CString status;
        const CString formatName(ZipEngine::FormatName(m_runFormat));
        status.Format(_T("Created %s (%s, %s, %u file(s), %s)."), fileName.GetString(), formatName.GetString(),
            FormatSize(size).GetString(), static_cast<unsigned>(m_runFileCount),
            m_runEncrypted ? _T("AES-256 encrypted") : _T("not encrypted"));
        SetDlgItemText(IDC_STATUS, status);

        CString headline;
        if (m_runEncrypted)
        {
            headline = _T("Encrypted ZIP archive created successfully!");
        }
        else if (m_runFormat == ZipEngine::ArchiveFormat::Zip)
        {
            headline = _T("ZIP archive created successfully (not encrypted).");
        }
        else
        {
            headline = formatName + _T(" archive created successfully.");
        }

        if (AfxMessageBox(headline + _T("\n\n") + m_runOutput +
            _T("\n\nOpen the folder that contains it?"),
            MB_YESNO | MB_ICONINFORMATION) == IDYES)
        {
            const CString args = _T("/select,\"") + m_runOutput + _T("\"");
            ShellExecute(GetSafeHwnd(), _T("open"), _T("explorer.exe"), args, nullptr, SW_SHOWNORMAL);
        }

        return 0;
    }

    m_progress.SetPos(0);
    if (result->status == ZipEngine::Status::Cancelled)
    {
        SetDlgItemText(IDC_STATUS, _T("Cancelled. No archive was written."));
        return 0;
    }

    const CString detail = FromUtf8(result->message);
    SetDlgItemText(IDC_STATUS, _T("Failed: ") + detail);
    AfxMessageBox(_T("Failed to create the ") + CString(ZipEngine::FormatName(m_runFormat)) + _T(" archive.\n\n") +
        CString(ZipEngine::StatusToString(result->status)) +
        _T(":\n") + detail,
        MB_ICONERROR);
    return 0;
}

void CZipAppMFCDlg::OnBnClickedBtnStop()
{
    m_cancel = true;   // the worker sees this at its next progress callback
    SetDlgItemText(IDC_STATUS, _T("Cancelling..."));
}

void CZipAppMFCDlg::OnCancel()
{
    if (m_pWorker)
    {
        if (AfxMessageBox(_T("A ZIP file is being created. Stop and close?"), MB_YESNO | MB_ICONQUESTION) != IDYES)
        {
            return;
        }

        m_cancel = true;
    }

    CDialogEx::OnCancel();
}

void CZipAppMFCDlg::OnDestroy()
{
    // Never leave the worker running against a destroyed window.
    if (m_pWorker)
    {
        m_cancel = true;
        WaitForSingleObject(m_pWorker->m_hThread, INFINITE);
        delete m_pWorker;
        m_pWorker = nullptr;

        // Free a result the worker may have posted after the dialog stopped processing messages.
        MSG msg;
        while (::PeekMessage(&msg, GetSafeHwnd(), WM_ZIP_DONE, WM_ZIP_DONE, PM_REMOVE))
        {
            delete reinterpret_cast<ZipResult*>(msg.lParam);
        }
    }

    CDialogEx::OnDestroy();
}
