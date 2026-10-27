// ZipAppMFC.h : application class for the archive tool.
#pragma once

#ifndef __AFXWIN_H__
#error "include 'pch.h' before including this file for PCH"
#endif

#include "resource.h"

class CZipAppMFCApp : public CWinApp
{
public:
    CZipAppMFCApp();

    // Handles the command line, then shows the main dialog; the application ends when it closes.
    //   ZipAppMFC.exe [/add] <file>...   open with these files on the Create tab
    //   ZipAppMFC.exe /extract <archive> open this archive on the Extract tab
    //   ZipAppMFC.exe /register | /unregister   add or remove the Explorer menu entries
    //                                           (no window; exit code 0 = done, 1 = failed)
    BOOL InitInstance() override;

    // Process exit code. MFC's default returns the last message's wParam (IDCANCEL = 2 when
    // the window is closed), which scripts would read as a failure; report m_exitCode instead.
    int ExitInstance() override;

    // Called by the main window once it can receive requests from other instances.
    void MainWindowReady();

    DECLARE_MESSAGE_MAP()

private:
    int    m_exitCode = 0;
    HANDLE m_startupMutex = nullptr;   // held while a window started from Explorer is opening
};

extern CZipAppMFCApp theApp;
