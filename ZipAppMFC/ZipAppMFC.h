// ZipAppMFC.h : application class for the ZIP creation tool.
#pragma once

#ifndef __AFXWIN_H__
#error "include 'pch.h' before including this file for PCH"
#endif

#include "resource.h"

class CZipAppMFCApp : public CWinApp
{
public:
    CZipAppMFCApp();

    // Shows the main dialog; the application ends when it closes.
    BOOL InitInstance() override;

    // Process exit code. MFC's default returns the last message's wParam (IDCANCEL = 2 when
    // the window is closed), which scripts would read as a failure; always report 0.
    int ExitInstance() override;

    DECLARE_MESSAGE_MAP()
};

extern CZipAppMFCApp theApp;
