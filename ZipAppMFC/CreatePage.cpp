// CreatePage.cpp : the "Create archive" tab of the main window.
//
// Threading: "Create Archive" copies everything the job needs into a ZipJob and starts
// ZipWorker on a background thread. The worker never touches MFC objects; it only posts
// WM_ZIP_PROGRESS (progress in tenths of a percent) and finally WM_ZIP_DONE (with a
// heap-allocated ZipResult that the UI thread deletes).
//
// Strings: MFC uses UTF-16 (CString in a Unicode build); ZipEngine uses UTF-8. Conversion
// happens only at the boundary, with CW2A/CA2W and CP_UTF8, so any file name or password
// character survives (the runbook's CT2A would use the ANSI code page and lose them).

#include "pch.h"
#include "ZipAppMFC.h"
#include "CreatePage.h"
#include "UiHelpers.h"
#include "../ZipEngineLib/ZipEngine.h"

#include <shellapi.h>

using namespace ui;

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
        ZipEngine::ZipEncryption encryption;
    };

    struct ZipResult
    {
        ZipEngine::Status status;
        std::string       message;   // UTF-8
    };

    // Combo box entries, in display order.
    struct FormatChoice
    {
        ZipEngine::ArchiveFormat format;
        LPCTSTR                  label;
    };

    const FormatChoice kFormats[] =
    {
        { ZipEngine::ArchiveFormat::Zip,    _T("ZIP (.zip) - can be password-protected") },
        { ZipEngine::ArchiveFormat::TarGz,  _T("TAR.GZ (.tar.gz) - gzip, widely used") },
        { ZipEngine::ArchiveFormat::TarXz,  _T("TAR.XZ (.tar.xz) - xz, smallest") },
        { ZipEngine::ArchiveFormat::TarBz2, _T("TAR.BZ2 (.tar.bz2) - bzip2") },
        { ZipEngine::ArchiveFormat::Tar,    _T("TAR (.tar) - not compressed") },
    };

}

BEGIN_MESSAGE_MAP(CCreatePage, CDialogEx)
    ON_BN_CLICKED(IDC_BTN_BROWSE, &CCreatePage::OnBnClickedBtnBrowse)
    ON_BN_CLICKED(IDC_BTN_REMOVE, &CCreatePage::OnBnClickedBtnRemove)
    ON_BN_CLICKED(IDC_BTN_CLEAR, &CCreatePage::OnBnClickedBtnClear)
    ON_BN_CLICKED(IDC_BTN_OUTPUT, &CCreatePage::OnBnClickedBtnOutput)
    ON_BN_CLICKED(IDC_BTN_ZIP, &CCreatePage::OnBnClickedBtnZip)
    ON_BN_CLICKED(IDC_BTN_STOP, &CCreatePage::OnBnClickedBtnStop)
    ON_LBN_SELCHANGE(IDC_LIST_FILES, &CCreatePage::OnLbnSelchangeListFiles)
    ON_CBN_SELCHANGE(IDC_COMBO_FORMAT, &CCreatePage::OnCbnSelchangeComboFormat)
    ON_BN_CLICKED(IDC_CHECK_LEGACY, &CCreatePage::UpdateControls)
    ON_WM_DESTROY()
    ON_MESSAGE(WM_ZIP_PROGRESS, &CCreatePage::OnZipProgress)
    ON_MESSAGE(WM_ZIP_DONE, &CCreatePage::OnZipDone)
END_MESSAGE_MAP()

CCreatePage::CCreatePage(CWnd* pParent)
    : CDialogEx(IDD_PAGE_CREATE, pParent)
{
}

void CCreatePage::DoDataExchange(CDataExchange* pDX)
{
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_LIST_FILES, m_listFiles);
    DDX_Control(pDX, IDC_COMBO_FORMAT, m_comboFormat);
    DDX_Control(pDX, IDC_PROGRESS, m_progress);
}

BOOL CCreatePage::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    m_progress.SetRange32(0, 1000);

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

int CCreatePage::AddFiles(const std::vector<CString>& paths)
{
    if (m_pWorker)   // ignore drops while an archive is being written
    {
        return 0;
    }

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
void CCreatePage::SuggestOutputPath()
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

ZipEngine::ArchiveFormat CCreatePage::CurrentFormat() const
{
    const int sel = m_comboFormat.GetCurSel();
    return sel == CB_ERR ? ZipEngine::ArchiveFormat::Zip
        : static_cast<ZipEngine::ArchiveFormat>(m_comboFormat.GetItemData(sel));
}

void CCreatePage::SelectFormat(ZipEngine::ArchiveFormat format)
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

void CCreatePage::ApplyFormatExtension()
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

void CCreatePage::OnCbnSelchangeComboFormat()
{
    ApplyFormatExtension();
    UpdateControls();
}

void CCreatePage::OnBnClickedBtnBrowse()
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

void CCreatePage::OnBnClickedBtnRemove()
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

void CCreatePage::OnBnClickedBtnClear()
{
    m_listFiles.ResetContent();
    m_listFiles.SetHorizontalExtent(0);
    UpdateControls();
}

void CCreatePage::OnLbnSelchangeListFiles()
{
    UpdateControls();
}

void CCreatePage::OnBnClickedBtnOutput()
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
void CCreatePage::UpdateControls()
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
    const bool legacy = IsDlgButtonChecked(IDC_CHECK_LEGACY) == BST_CHECKED;
    GetDlgItem(IDC_EDIT_PASS)->EnableWindow(!running && canEncrypt);
    GetDlgItem(IDC_EDIT_PASS_CONFIRM)->EnableWindow(!running && canEncrypt);
    GetDlgItem(IDC_CHECK_LEGACY)->EnableWindow(!running && canEncrypt);
    SetDlgItemText(IDC_ENCRYPTION_NOTE, !canEncrypt
        ? _T("TAR archives cannot be password-protected. Choose ZIP if the files must be encrypted. ")
        _T("Open with 7-Zip, WinRAR, or tar (built into Windows 10 and later).")
        : legacy
        ? _T("Legacy ZipCrypto: Windows Explorer and almost any ZIP program can open it with the password, but this ")
        _T("old encryption is weak: free tools can recover the files, often within hours. Do not use it for sensitive data.")
        : _T("Strong AES-256 encryption. Open the archive with this app's Extract tab, 7-Zip, WinRAR or WinZip; ")
        _T("Windows Explorer cannot open AES-encrypted ZIP files. Use English letters, digits and punctuation in the password."));

    GetDlgItem(IDC_BTN_ZIP)->EnableWindow(!running && hasFiles);
    GetDlgItem(IDC_BTN_STOP)->EnableWindow(running);
    m_listFiles.EnableWindow(!running);
}

// ---------------------------------------------------------------------------
// Creating the archive

// Handler for "Create Archive": validates the form, then starts the worker thread.
void CCreatePage::OnBnClickedBtnZip()
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
    job->encryption = IsDlgButtonChecked(IDC_CHECK_LEGACY) == BST_CHECKED
        ? ZipEngine::ZipEncryption::ZipCrypto : ZipEngine::ZipEncryption::Aes256;

    m_runOutput = output;
    m_runFileCount = job->files.size();
    m_runEncrypted = !job->password.empty();
    m_runLegacy = job->encryption == ZipEngine::ZipEncryption::ZipCrypto;
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
UINT __cdecl CCreatePage::ZipWorker(LPVOID pParam)
{
    ZipJob* job = static_cast<ZipJob*>(pParam);
    int lastPermille = -1;

    // One call for every format: ZIP goes to ZipCompressor, TAR formats to TarCompressor.
    std::string errorMessage;
    const ZipEngine::Status status = ZipEngine::CreateArchive(
        job->format, job->output, job->files, job->password, job->encryption,
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

LRESULT CCreatePage::OnZipProgress(WPARAM wParam, LPARAM)
{
    m_progress.SetPos(static_cast<int>(wParam));
    CString text;
    text.Format(_T("Compressing%s... %d%%"), m_runEncrypted ? _T(" and encrypting") : _T(""), static_cast<int>(wParam / 10));
    SetDlgItemText(IDC_STATUS, text);
    return 0;
}

LRESULT CCreatePage::OnZipDone(WPARAM, LPARAM lParam)
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
            !m_runEncrypted ? _T("not encrypted") : m_runLegacy ? _T("ZipCrypto encrypted") : _T("AES-256 encrypted"));
        SetDlgItemText(IDC_STATUS, status);

        CString headline;
        if (m_runEncrypted && m_runLegacy)
        {
            headline = _T("Encrypted ZIP archive created with legacy ZipCrypto encryption.\n\n")
                _T("Windows Explorer can open it with the password. Remember that this encryption is weak.");
        }
        else if (m_runEncrypted)
        {
            headline = _T("Encrypted ZIP archive created successfully (AES-256)!");
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

void CCreatePage::OnBnClickedBtnStop()
{
    m_cancel = true;   // the worker sees this at its next progress callback
    SetDlgItemText(IDC_STATUS, _T("Cancelling..."));
}

void CCreatePage::OnDestroy()
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
