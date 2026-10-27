// ZipAppMFCDlg.h : main window. A tab control with two pages: "Create archive" (CCreatePage)
// and "Extract archive" (CExtractPage). The window owns the icon, drag-and-drop, the Explorer
// menu button and Close; each page does its own work on its own worker thread.
#pragma once

#include "CreatePage.h"
#include "ExtractPage.h"

// What Explorer's right-click menu asked for (ZipAppMFC.exe /add or /extract), either on the
// command line or forwarded from a second instance with WM_COPYDATA.
struct ShellRequest
{
    enum Kind
    {
        None = 0,
        Add = 1,
        Extract = 2
    };

    Kind                 kind = None;
    std::vector<CString> paths;
};

class CZipAppMFCDlg : public CDialogEx
{
public:
    explicit CZipAppMFCDlg(CWnd* pParent = nullptr);

    enum { IDD = IDD_ZIPAPPMFC_DIALOG };

    // Window property that marks the main window, so a second instance can find it.
    static const LPCWSTR kWindowProp;

    // Applied once the window is shown.
    void SetStartupRequest(const ShellRequest& request)
    {
        m_startup = request;
    }

protected:
    void DoDataExchange(CDataExchange* pDX) override;
    BOOL OnInitDialog() override;
    void OnOK() override;         // Enter: presses the shown page's default button
    void OnCancel() override;     // Close button / Escape / title-bar X

    afx_msg void OnTcnSelchangeTabs(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnDropFiles(HDROP hDropInfo);
    afx_msg BOOL OnCopyData(CWnd* pWnd, COPYDATASTRUCT* pCopyDataStruct);
    afx_msg LRESULT OnApplyPending(WPARAM wParam, LPARAM lParam);
    afx_msg void OnBnClickedBtnShell();
    afx_msg void OnDestroy();
    DECLARE_MESSAGE_MAP()

private:
    enum Page
    {
        pageCreate,
        pageExtract
    };

    // Sizes the tab control around the pages and the window around the tab control.
    void LayoutPages();
    void ShowPage(int page);
    CDialogEx* ActivePage();
    void ApplyRequest(const ShellRequest& request);

    // "Add to Explorer menu" or "Remove from Explorer menu", depending on the registry.
    void UpdateShellButton();

    CTabCtrl     m_tabs;
    CCreatePage  m_createPage;
    CExtractPage m_extractPage;
    HICON        m_hIcon = nullptr;
    ShellRequest m_startup;
    std::vector<ShellRequest> m_pending;   // forwarded requests, applied by OnApplyPending
};
