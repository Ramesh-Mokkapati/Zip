// ZipAppMFCDlg.cpp : main window of the archive tool.
//
// The window is a tab control with two child dialogs on top of it, one per tab. Only the shown
// page is visible; both stay alive, so switching tabs keeps what was entered and a job keeps
// running in the background. Files dragged onto the window go to the shown page: the Create
// page adds them to its list, the Extract page opens the (first) archive.
//
// Explorer's right-click menu (see ShellIntegration.h) starts the app with /add or /extract.
// Explorer starts one process per selected file, so ZipAppMFC.cpp forwards each request to an
// already-open window with WM_COPYDATA, and OnCopyData collects them here.

#include "pch.h"
#include "ZipAppMFC.h"
#include "ZipAppMFCDlg.h"
#include "ShellIntegration.h"

#include <shellapi.h>
#include <uxtheme.h>

#pragma comment(lib, "uxtheme.lib")

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace
{
    const UINT WM_APPLY_PENDING = WM_APP + 20;   // applies requests queued by OnCopyData
}

BEGIN_MESSAGE_MAP(CZipAppMFCDlg, CDialogEx)
    ON_NOTIFY(TCN_SELCHANGE, IDC_TABS, &CZipAppMFCDlg::OnTcnSelchangeTabs)
    ON_WM_DROPFILES()
    ON_WM_COPYDATA()
    ON_MESSAGE(WM_APPLY_PENDING, &CZipAppMFCDlg::OnApplyPending)
    ON_BN_CLICKED(IDC_BTN_SHELL, &CZipAppMFCDlg::OnBnClickedBtnShell)
    ON_WM_DESTROY()
END_MESSAGE_MAP()

const LPCWSTR CZipAppMFCDlg::kWindowProp = L"ZipAppMFC.MainWindow";

CZipAppMFCDlg::CZipAppMFCDlg(CWnd* pParent)
    : CDialogEx(IDD_ZIPAPPMFC_DIALOG, pParent)
{
    m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

void CZipAppMFCDlg::DoDataExchange(CDataExchange* pDX)
{
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_TABS, m_tabs);
}

BOOL CZipAppMFCDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    SetIcon(m_hIcon, TRUE);
    SetIcon(m_hIcon, FALSE);

    m_tabs.InsertItem(pageCreate, _T("Create archive"));
    m_tabs.InsertItem(pageExtract, _T("Extract archive"));

    m_createPage.Create(CCreatePage::IDD, this);
    m_extractPage.Create(CExtractPage::IDD, this);

    // Draw the pages with the tab body's background, like a property sheet.
    EnableThemeDialogTexture(m_createPage.GetSafeHwnd(), ETDT_ENABLETAB);
    EnableThemeDialogTexture(m_extractPage.GetSafeHwnd(), ETDT_ENABLETAB);

    LayoutPages();
    UpdateShellButton();
    ShowPage(pageCreate);
    CenterWindow();

    // From now on, a second instance started by Explorer hands its files to this window.
    SetProp(GetSafeHwnd(), kWindowProp, reinterpret_cast<HANDLE>(1));
    theApp.MainWindowReady();

    // Applied once the window is on screen (a message box may follow).
    if (m_startup.kind != ShellRequest::None)
    {
        m_pending.push_back(m_startup);
        m_startup = ShellRequest();
        PostMessage(WM_APPLY_PENDING);
    }

    return FALSE;   // ShowPage set the focus
}

void CZipAppMFCDlg::LayoutPages()
{
    // The pages' own size (from their templates) decides everything else.
    CRect page;
    m_createPage.GetWindowRect(&page);
    page.MoveToXY(0, 0);

    CRect tabRect;
    m_tabs.GetWindowRect(&tabRect);
    ScreenToClient(&tabRect);

    CRect display(page);
    m_tabs.AdjustRect(TRUE, &display);   // page area -> whole tab control
    display.MoveToXY(tabRect.left, tabRect.top);
    m_tabs.MoveWindow(&display);

    CRect pageRect(display);
    m_tabs.AdjustRect(FALSE, &pageRect);   // whole tab control -> page area
    const UINT flags = SWP_NOACTIVATE;
    m_createPage.SetWindowPos(&wndTop, pageRect.left, pageRect.top, pageRect.Width(), pageRect.Height(), flags);
    m_extractPage.SetWindowPos(&wndTop, pageRect.left, pageRect.top, pageRect.Width(), pageRect.Height(), flags);

    // Close sits under the tab control's right edge, the Explorer menu button under its left
    // edge; the window wraps around them.
    CWnd* close = GetDlgItem(IDCANCEL);
    CRect closeRect;
    close->GetWindowRect(&closeRect);
    const int gap = tabRect.left;
    const int closeLeft = display.right - closeRect.Width();
    const int closeTop = display.bottom + gap;
    close->SetWindowPos(nullptr, closeLeft, closeTop, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    GetDlgItem(IDC_BTN_SHELL)->SetWindowPos(nullptr, display.left, closeTop, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    CRect client(0, 0, display.right + gap, closeTop + closeRect.Height() + gap);
    CalcWindowRect(&client, CWnd::adjustBorder);
    SetWindowPos(nullptr, 0, 0, client.Width(), client.Height(), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

CDialogEx* CZipAppMFCDlg::ActivePage()
{
    return m_tabs.GetCurSel() == pageExtract ? static_cast<CDialogEx*>(&m_extractPage)
        : static_cast<CDialogEx*>(&m_createPage);
}

void CZipAppMFCDlg::ShowPage(int page)
{
    m_tabs.SetCurSel(page);
    m_createPage.ShowWindow(page == pageCreate ? SW_SHOW : SW_HIDE);
    m_extractPage.ShowWindow(page == pageExtract ? SW_SHOW : SW_HIDE);

    // Focus the page's first control, so the keyboard works straight away.
    CDialogEx* active = ActivePage();
    if (CWnd* first = active->GetNextDlgTabItem(nullptr))
    {
        GotoDlgCtrl(first);
    }
}

void CZipAppMFCDlg::OnTcnSelchangeTabs(NMHDR*, LRESULT* pResult)
{
    ShowPage(m_tabs.GetCurSel());
    *pResult = 0;
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

    if (paths.empty())
    {
        return;
    }

    if (m_tabs.GetCurSel() == pageExtract)
    {
        m_extractPage.SetArchive(paths.front());
    }
    else
    {
        m_createPage.AddFiles(paths);
    }
}

void CZipAppMFCDlg::OnOK()
{
    // The pages are child windows, so Enter arrives here; pass it to the shown page's
    // default button (Create Archive / Extract) if that button is currently enabled.
    CDialogEx* page = ActivePage();
    const DWORD def = static_cast<DWORD>(page->SendMessage(DM_GETDEFID));
    if (HIWORD(def) == DC_HASDEFID)
    {
        CWnd* button = page->GetDlgItem(LOWORD(def));
        if (button && button->IsWindowEnabled())
        {
            page->SendMessage(WM_COMMAND, MAKEWPARAM(LOWORD(def), BN_CLICKED), reinterpret_cast<LPARAM>(button->GetSafeHwnd()));
        }
    }
}

void CZipAppMFCDlg::OnCancel()
{
    if (m_createPage.IsBusy() || m_extractPage.IsBusy())
    {
        const LPCTSTR what = m_createPage.IsBusy() ? _T("An archive is being created.") : _T("An archive is being extracted.");
        if (AfxMessageBox(CString(what) + _T(" Stop and close?"), MB_YESNO | MB_ICONQUESTION) != IDYES)
        {
            return;
        }

        // The pages wait for their workers in OnDestroy; an extraction that is stopped removes
        // the files it already wrote.
        m_createPage.RequestCancel();
        m_extractPage.RequestCancel();
    }

    CDialogEx::OnCancel();
}

// ---------------------------------------------------------------------------
// Explorer integration

void CZipAppMFCDlg::ApplyRequest(const ShellRequest& request)
{
    if (request.paths.empty())
    {
        return;
    }

    if (request.kind == ShellRequest::Add)
    {
        if (m_createPage.IsBusy())
        {
            AfxMessageBox(_T("An archive is being created. Add the files again when it has finished."), MB_ICONINFORMATION);
            return;
        }

        ShowPage(pageCreate);
        m_createPage.AddFiles(request.paths);
    }
    else if (request.kind == ShellRequest::Extract)
    {
        if (m_extractPage.IsBusy())
        {
            AfxMessageBox(_T("An archive is being extracted. Open the next one when it has finished."), MB_ICONINFORMATION);
            return;
        }

        ShowPage(pageExtract);
        m_extractPage.SetArchive(request.paths.front(), true);
    }
}

// A second instance forwards its request: dwData is the ShellRequest::Kind, lpData the paths
// as UTF-16 strings, each ending with a NUL.
BOOL CZipAppMFCDlg::OnCopyData(CWnd*, COPYDATASTRUCT* pCopyDataStruct)
{
    if (pCopyDataStruct->dwData != ShellRequest::Add && pCopyDataStruct->dwData != ShellRequest::Extract)
    {
        return FALSE;
    }

    ShellRequest request;
    request.kind = static_cast<ShellRequest::Kind>(pCopyDataStruct->dwData);
    const wchar_t* p = static_cast<const wchar_t*>(pCopyDataStruct->lpData);
    const wchar_t* end = p + pCopyDataStruct->cbData / sizeof(wchar_t);
    while (p < end)
    {
        const wchar_t* nul = std::find(p, end, L'\0');
        if (nul > p)
        {
            request.paths.push_back(CString(p, static_cast<int>(nul - p)));
        }

        p = nul + 1;
    }

    // The sender waits for this handler, so apply the request after returning: a message box
    // shown here would otherwise keep the second instance (and Explorer's launch) waiting.
    m_pending.push_back(request);
    PostMessage(WM_APPLY_PENDING);
    return TRUE;
}

LRESULT CZipAppMFCDlg::OnApplyPending(WPARAM, LPARAM)
{
    // The request came from the user's click in Explorer: come to the front.
    if (IsIconic())
    {
        ShowWindow(SW_RESTORE);
    }

    SetForegroundWindow();

    std::vector<ShellRequest> pending;
    pending.swap(m_pending);
    for (const ShellRequest& request : pending)
    {
        ApplyRequest(request);
    }

    return 0;
}

void CZipAppMFCDlg::UpdateShellButton()
{
    SetDlgItemText(IDC_BTN_SHELL, shell::GetState() == shell::State::Registered
        ? _T("Remove from Explorer menu") : _T("Add to Explorer menu"));
}

void CZipAppMFCDlg::OnBnClickedBtnShell()
{
    if (shell::GetState() == shell::State::Registered)
    {
        if (shell::Unregister())
        {
            AfxMessageBox(_T("The Explorer menu entries were removed."), MB_ICONINFORMATION);
        }
        else
        {
            AfxMessageBox(_T("Some Explorer menu entries could not be removed."), MB_ICONWARNING);
        }
    }
    else
    {
        CString error;
        if (shell::Register(error))
        {
            AfxMessageBox(_T("Added to Explorer's right-click menu, for your user account:\n\n")
                _T("  • Any file: \"Add to archive (ZIP / TAR)...\"\n")
                _T("  • .zip, .tar, .tgz, .tar.gz, .tar.bz2, .tar.xz ...: \"Extract with ZIP / TAR Archive Tool...\"\n\n")
                _T("On Windows 11 they are under \"Show more options\" (or Shift + right-click).\n\n")
                _T("The entries point to this copy of the program; if you move it, add them again from here."),
                MB_ICONINFORMATION);
        }
        else
        {
            AfxMessageBox(error, MB_ICONERROR);
        }
    }

    UpdateShellButton();
}

void CZipAppMFCDlg::OnDestroy()
{
    RemoveProp(GetSafeHwnd(), kWindowProp);
    CDialogEx::OnDestroy();
}
