// ExtractPage.cpp : the "Extract archive" tab of the main window.
//
// Choosing an archive (Browse, typing/pasting a path, or dragging a file onto the window)
// lists its contents straight away, so the user can see what it holds and whether a password
// is needed before extracting. Extraction runs on a worker thread exactly like creation in
// CreatePage: the worker only posts WM_X_PROGRESS and finally WM_X_DONE.
//
// The engine refuses unsafe archives (entries that would escape the folder, device names, ...)
// before writing anything; this page explains such refusals instead of just reporting "failed".

#include "pch.h"
#include "ZipAppMFC.h"
#include "ExtractPage.h"
#include "UiHelpers.h"

#include <shellapi.h>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

using namespace ui;

namespace
{
    const UINT WM_X_PROGRESS = WM_APP + 11;   // wParam = progress 0..1000
    const UINT WM_X_DONE = WM_APP + 12;       // lParam = ExtractResult* (receiver deletes)
    const UINT_PTR kLoadTimer = 1;            // reloads the contents shortly after the path stops changing

    enum Column
    {
        colName,
        colSize,
        colEncrypted
    };

    struct ExtractJob
    {
        HWND               hwnd;
        std::atomic<bool>* cancel;
        std::string        archive;    // UTF-8
        std::string        dest;       // UTF-8
        std::string        password;   // UTF-8, wiped when the job ends
        bool               overwrite;
    };

    struct ExtractResult
    {
        ZipEngine::Status         status;
        std::string               message;
        ZipEngine::ExtractSummary summary;
    };
}

BEGIN_MESSAGE_MAP(CExtractPage, CDialogEx)
    ON_BN_CLICKED(IDC_BTN_ARCHIVE, &CExtractPage::OnBnClickedBtnArchive)
    ON_BN_CLICKED(IDC_BTN_DEST, &CExtractPage::OnBnClickedBtnDest)
    ON_BN_CLICKED(IDC_BTN_EXTRACT, &CExtractPage::OnBnClickedBtnExtract)
    ON_BN_CLICKED(IDC_BTN_XSTOP, &CExtractPage::OnBnClickedBtnStop)
    ON_EN_CHANGE(IDC_EDIT_ARCHIVE, &CExtractPage::OnEnChangeArchive)
    ON_EN_CHANGE(IDC_EDIT_DEST, &CExtractPage::OnEnChangeDest)
    ON_WM_TIMER()
    ON_WM_DESTROY()
    ON_MESSAGE(WM_X_PROGRESS, &CExtractPage::OnExtractProgress)
    ON_MESSAGE(WM_X_DONE, &CExtractPage::OnExtractDone)
END_MESSAGE_MAP()

CExtractPage::CExtractPage(CWnd* pParent)
    : CDialogEx(IDD_PAGE_EXTRACT, pParent)
{
}

void CExtractPage::DoDataExchange(CDataExchange* pDX)
{
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_LIST_CONTENTS, m_listContents);
    DDX_Control(pDX, IDC_XPROGRESS, m_progress);
}

BOOL CExtractPage::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    m_progress.SetRange32(0, 1000);

    m_listContents.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    CRect rc;
    m_listContents.GetClientRect(&rc);
    m_listContents.InsertColumn(colName, _T("Name"), LVCFMT_LEFT, rc.Width() * 64 / 100);
    m_listContents.InsertColumn(colSize, _T("Size"), LVCFMT_RIGHT, rc.Width() * 18 / 100);
    m_listContents.InsertColumn(colEncrypted, _T("Encrypted"), LVCFMT_LEFT, rc.Width() * 14 / 100);

    SetDlgItemText(IDC_CONTENTS_INFO, _T("Choose an archive with Browse, or drag a .zip or .tar file onto this window."));
    SetDlgItemText(IDC_XSTATUS, _T(""));
    UpdateControls();
    return TRUE;
}

// ---------------------------------------------------------------------------
// Choosing the archive and the folder

void CExtractPage::SetArchive(const CString& path, bool fromExplorer)
{
    if (m_pWorker)
    {
        return;
    }

    if (fromExplorer)
    {
        m_destIsSuggested = true;
        m_loadedArchive.Empty();   // reload even if it is already shown (it may have changed)
    }

    SetDlgItemText(IDC_EDIT_ARCHIVE, path);   // triggers OnEnChangeArchive
    KillTimer(kLoadTimer);
    LoadArchive();

    if (fromExplorer && m_loadedOk)
    {
        // The page is a child of the main window, which owns the dialog focus.
        CWnd* next = GetDlgItem(m_anyEncrypted ? IDC_EDIT_XPASS : IDC_BTN_EXTRACT);
        GetParent()->SendMessage(WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(next->GetSafeHwnd()), TRUE);
    }
}

void CExtractPage::OnBnClickedBtnArchive()
{
    CString current;
    GetDlgItemText(IDC_EDIT_ARCHIVE, current);
    CFileDialog dlg(TRUE, nullptr, current, OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
        _T("Archives (*.zip;*.tar;*.tar.gz;*.tgz;*.tar.bz2;*.tbz2;*.tbz;*.tar.xz;*.txz)|")
        _T("*.zip;*.tar;*.tar.gz;*.tgz;*.tar.bz2;*.tbz2;*.tbz;*.tar.xz;*.txz|All files (*.*)|*.*||"), this);
    if (dlg.DoModal() == IDOK)
    {
        SetArchive(dlg.GetPathName());
    }
}

// Typing or pasting a path: reload once the text has been still for a moment, rather than on
// every keystroke.
void CExtractPage::OnEnChangeArchive()
{
    SetTimer(kLoadTimer, 400, nullptr);
}

void CExtractPage::OnTimer(UINT_PTR nIDEvent)
{
    if (nIDEvent == kLoadTimer)
    {
        KillTimer(kLoadTimer);
        LoadArchive();
        return;
    }

    CDialogEx::OnTimer(nIDEvent);
}

void CExtractPage::OnEnChangeDest()
{
    if (!m_settingDest)
    {
        m_destIsSuggested = false;   // the user's own choice is never replaced
    }
}

void CExtractPage::OnBnClickedBtnDest()
{
    CString current;
    GetDlgItemText(IDC_EDIT_DEST, current);
    CFolderPickerDialog dlg(current, 0, this);
    if (dlg.DoModal() == IDOK)
    {
        SetDlgItemText(IDC_EDIT_DEST, dlg.GetPathName());
    }
}

void CExtractPage::LoadArchive()
{
    CString archive;
    GetDlgItemText(IDC_EDIT_ARCHIVE, archive);
    archive.Trim();
    archive.Trim(_T('"'));   // paths pasted from "Copy as path" are quoted
    if (archive == m_loadedArchive)
    {
        return;
    }

    m_loadedArchive = archive;
    m_loadedOk = false;
    m_anyEncrypted = false;
    m_listContents.DeleteAllItems();

    if (archive.IsEmpty())
    {
        SetDlgItemText(IDC_CONTENTS_INFO, _T("Choose an archive with Browse, or drag a .zip or .tar file onto this window."));
        UpdateControls();
        return;
    }

    const DWORD attrs = GetFileAttributes(archive);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY))
    {
        SetDlgItemText(IDC_CONTENTS_INFO, _T("File not found."));
        UpdateControls();
        return;
    }

    std::vector<ZipEngine::ArchiveEntryInfo> entries;
    std::string error;
    if (ZipEngine::ListArchive(ToUtf8(archive), entries, error) != ZipEngine::Status::Success)
    {
        SetDlgItemText(IDC_CONTENTS_INFO, _T("This is not a ZIP or TAR archive, or it is damaged."));
        UpdateControls();
        return;
    }

    m_listContents.SetRedraw(FALSE);
    unsigned files = 0, folders = 0;
    ULONGLONG total = 0;
    for (const auto& e : entries)
    {
        const int row = m_listContents.InsertItem(m_listContents.GetItemCount(), FromUtf8(e.name));
        m_listContents.SetItemText(row, colSize, e.isDirectory ? CString(_T("folder")) : FormatSize(e.size));
        m_listContents.SetItemText(row, colEncrypted, e.encrypted ? _T("Yes") : _T(""));
        if (e.isDirectory)
        {
            ++folders;
        }
        else
        {
            ++files;
            total += e.size;
        }

        m_anyEncrypted = m_anyEncrypted || e.encrypted;
    }

    m_listContents.SetRedraw(TRUE);
    m_loadedOk = true;

    CString info;
    info.Format(_T("%u file(s)"), files);
    if (folders)
    {
        info.AppendFormat(_T(" in %u folder(s)"), folders);
    }

    info.AppendFormat(_T(", %s in total."), FormatSize(total).GetString());
    if (m_anyEncrypted)
    {
        info += _T("  Encrypted: enter the password used to create it.");
    }

    SetDlgItemText(IDC_CONTENTS_INFO, info);

    // Suggest "<archive folder>\<archive name>" until the user picks a folder themselves.
    if (m_destIsSuggested)
    {
        m_settingDest = true;
        SetDlgItemText(IDC_EDIT_DEST, WithoutArchiveExtension(archive));
        m_settingDest = false;
    }

    UpdateControls();
}

void CExtractPage::UpdateControls()
{
    const bool running = m_pWorker != nullptr;
    GetDlgItem(IDC_EDIT_ARCHIVE)->EnableWindow(!running);
    GetDlgItem(IDC_BTN_ARCHIVE)->EnableWindow(!running);
    GetDlgItem(IDC_EDIT_DEST)->EnableWindow(!running);
    GetDlgItem(IDC_BTN_DEST)->EnableWindow(!running);
    GetDlgItem(IDC_EDIT_XPASS)->EnableWindow(!running && m_anyEncrypted);
    GetDlgItem(IDC_CHECK_OVERWRITE)->EnableWindow(!running);
    GetDlgItem(IDC_BTN_EXTRACT)->EnableWindow(!running && m_loadedOk);
    GetDlgItem(IDC_BTN_XSTOP)->EnableWindow(running);
}

// ---------------------------------------------------------------------------
// Extracting

void CExtractPage::OnBnClickedBtnExtract()
{
    if (m_pWorker)
    {
        return;
    }

    KillTimer(kLoadTimer);
    LoadArchive();   // make sure the list matches the path box

    if (!m_loadedOk)
    {
        AfxMessageBox(_T("Choose a ZIP or TAR archive first."), MB_ICONINFORMATION);
        return;
    }

    CString dest, password;
    GetDlgItemText(IDC_EDIT_DEST, dest);
    dest.Trim();
    dest.Trim(_T('"'));
    if (dest.IsEmpty())
    {
        AfxMessageBox(_T("Choose the folder to extract into."), MB_ICONINFORMATION);
        GetDlgItem(IDC_EDIT_DEST)->SetFocus();
        return;
    }

    if (m_anyEncrypted)
    {
        GetDlgItemText(IDC_EDIT_XPASS, password);
        if (password.IsEmpty())
        {
            AfxMessageBox(_T("This archive is encrypted. Enter the password that was used to create it."), MB_ICONINFORMATION);
            GetDlgItem(IDC_EDIT_XPASS)->SetFocus();
            return;
        }
    }

    ExtractJob* job = new ExtractJob;
    job->hwnd = GetSafeHwnd();
    job->cancel = &m_cancel;
    job->archive = ToUtf8(m_loadedArchive);
    job->dest = ToUtf8(dest);
    job->password = ToUtf8(password);
    job->overwrite = IsDlgButtonChecked(IDC_CHECK_OVERWRITE) == BST_CHECKED;
    Wipe(password);

    m_runDest = dest;
    m_cancel = false;
    m_pWorker = AfxBeginThread(ExtractWorker, job, THREAD_PRIORITY_BELOW_NORMAL, 0, CREATE_SUSPENDED);
    if (!m_pWorker)
    {
        Wipe(job->password);
        delete job;
        AfxMessageBox(_T("Could not start the background thread."), MB_ICONERROR);
        return;
    }

    m_pWorker->m_bAutoDelete = FALSE;   // waited on and deleted in OnExtractDone / OnDestroy
    m_pWorker->ResumeThread();

    m_progress.SetPos(0);
    SetDlgItemText(IDC_XSTATUS, _T("Checking the archive..."));
    UpdateControls();
}

// Runs on the worker thread; reports back only through PostMessage.
UINT __cdecl CExtractPage::ExtractWorker(LPVOID pParam)
{
    ExtractJob* job = static_cast<ExtractJob*>(pParam);
    int lastPermille = -1;

    ExtractResult* result = new ExtractResult;
    result->status = ZipEngine::ExtractArchive(
        job->archive, job->dest, job->password, job->overwrite,
        [job, &lastPermille](double progress) -> bool
    {
        const int permille = static_cast<int>(progress * 1000.0);
        if (permille != lastPermille)
        {
            lastPermille = permille;
            ::PostMessage(job->hwnd, WM_X_PROGRESS, static_cast<WPARAM>(permille), 0);
        }
        return !job->cancel->load();
    },
        result->summary, result->message);

    const HWND hwnd = job->hwnd;
    Wipe(job->password);
    delete job;

    if (!::PostMessage(hwnd, WM_X_DONE, 0, reinterpret_cast<LPARAM>(result)))
    {
        delete result;   // window already gone
    }

    return 0;
}

LRESULT CExtractPage::OnExtractProgress(WPARAM wParam, LPARAM)
{
    m_progress.SetPos(static_cast<int>(wParam));
    CString text;
    text.Format(_T("Extracting... %d%%"), static_cast<int>(wParam / 10));
    SetDlgItemText(IDC_XSTATUS, text);
    return 0;
}

LRESULT CExtractPage::OnExtractDone(WPARAM, LPARAM lParam)
{
    std::unique_ptr<ExtractResult> result(reinterpret_cast<ExtractResult*>(lParam));

    if (m_pWorker)
    {
        WaitForSingleObject(m_pWorker->m_hThread, INFINITE);
        delete m_pWorker;
        m_pWorker = nullptr;
    }

    UpdateControls();
    const CString detail = FromUtf8(result->message);

    switch (result->status)
    {

    case ZipEngine::Status::Success:
    {
        SetProgressInstant(m_progress, 1000);
        const ZipEngine::ExtractSummary& s = result->summary;
        CString status;
        status.Format(_T("Extracted %u file(s), %s."), s.files, FormatSize(s.bytes).GetString());
        if (!s.skipped.empty())
        {
            status.AppendFormat(_T(" Skipped %u link(s)/special entries."), static_cast<unsigned>(s.skipped.size()));
        }

        SetDlgItemText(IDC_XSTATUS, status);

        CString message = status + _T("\n\n") + m_runDest;
        if (!s.skipped.empty())
        {
            message += _T("\n\nLinks and special entries are never extracted, because they could point outside the folder:\n");
            for (size_t i = 0; i < s.skipped.size() && i < 10; ++i)
            {
                message += _T("  ") + FromUtf8(s.skipped[i]) + _T("\n");
            }
        }

        if (AfxMessageBox(message + _T("\n\nOpen the folder?"), MB_YESNO | MB_ICONINFORMATION) == IDYES)
        {
            ShellExecute(GetSafeHwnd(), _T("open"), _T("explorer.exe"), _T("\"") + m_runDest + _T("\""), nullptr, SW_SHOWNORMAL);
        }

        return 0;
    }

    case ZipEngine::Status::Cancelled:
        m_progress.SetPos(0);
        SetDlgItemText(IDC_XSTATUS, _T("Cancelled. The files extracted so far were removed."));
        return 0;

    case ZipEngine::Status::PasswordRequired:
    case ZipEngine::Status::WrongPassword:
        m_progress.SetPos(0);
        SetDlgItemText(IDC_XSTATUS, _T("Wrong password. Nothing was extracted."));
        AfxMessageBox(detail + _T("\n\nCheck Caps Lock and the keyboard layout, then try again."), MB_ICONWARNING);
        SetDlgItemText(IDC_EDIT_XPASS, _T(""));
        GetDlgItem(IDC_EDIT_XPASS)->SetFocus();
        return 0;

    case ZipEngine::Status::FileExists:
        m_progress.SetPos(0);
        SetDlgItemText(IDC_XSTATUS, _T("Nothing was extracted: some files already exist."));
        AfxMessageBox(detail + _T("\n\nTick \"Replace existing files\" to overwrite them."), MB_ICONINFORMATION);
        return 0;

    case ZipEngine::Status::UnsafeEntry:
        m_progress.SetPos(0);
        SetDlgItemText(IDC_XSTATUS, _T("Refused: the archive contains an unsafe entry. Nothing was extracted."));
        AfxMessageBox(_T("This archive was not extracted because it could write files outside the chosen folder, ")
            _T("or uses names Windows cannot handle safely. This is how malicious archives try to overwrite ")
            _T("other files.\n\n") + detail, MB_ICONWARNING);
        return 0;

    default:
        m_progress.SetPos(0);
        SetDlgItemText(IDC_XSTATUS, _T("Failed: ") + detail);
        AfxMessageBox(_T("Failed to extract the archive.\n\n") + CString(ZipEngine::StatusToString(result->status)) +
            _T(":\n") + detail, MB_ICONERROR);
        return 0;
    }
}

void CExtractPage::OnBnClickedBtnStop()
{
    m_cancel = true;
    SetDlgItemText(IDC_XSTATUS, _T("Cancelling..."));
}

void CExtractPage::OnDestroy()
{
    KillTimer(kLoadTimer);
    if (m_pWorker)
    {
        m_cancel = true;
        WaitForSingleObject(m_pWorker->m_hThread, INFINITE);
        delete m_pWorker;
        m_pWorker = nullptr;

        MSG msg;
        while (::PeekMessage(&msg, GetSafeHwnd(), WM_X_DONE, WM_X_DONE, PM_REMOVE))
        {
            delete reinterpret_cast<ExtractResult*>(msg.lParam);
        }
    }

    CDialogEx::OnDestroy();
}
