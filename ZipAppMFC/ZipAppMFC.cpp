// ZipAppMFC.cpp : application entry point. A dialog-based MFC app: InitInstance shows the
// dialog modally and returns FALSE so the app exits when the dialog closes.

#include "pch.h"
#include "ZipAppMFC.h"
#include "ZipAppMFCDlg.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

BEGIN_MESSAGE_MAP(CZipAppMFCApp, CWinApp)
END_MESSAGE_MAP()

CZipAppMFCApp::CZipAppMFCApp()
{
}

CZipAppMFCApp theApp;   // the one and only application object

BOOL CZipAppMFCApp::InitInstance()
{
    // Register the common-control classes (progress bar etc.) for visual styles.
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);

    CWinApp::InitInstance();

#ifdef _DEBUG
    // Report CRT memory leaks in the debugger's Output window on exit.
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    CZipAppMFCDlg dlg;
    m_pMainWnd = &dlg;
    dlg.DoModal();

    // Dialog closed: returning FALSE ends the application instead of starting a message pump.
    return FALSE;
}

int CZipAppMFCApp::ExitInstance()
{
    CWinApp::ExitInstance();
    return 0;
}
