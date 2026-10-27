// ZipAppMFC.cpp : application entry point. A dialog-based MFC app: InitInstance shows the
// dialog modally and returns FALSE so the app exits when the dialog closes.
//
// Explorer's right-click menu runs "ZipAppMFC.exe /add <file>" once per selected file, all at
// nearly the same moment. So that they end up in one window, each such start takes a named
// mutex: the first one opens the window (and releases the mutex once the window is ready),
// later ones find that window and send it their file with WM_COPYDATA, then exit.

#include "pch.h"
#include "ZipAppMFC.h"
#include "ZipAppMFCDlg.h"
#include "ShellIntegration.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace
{
    const LPCWSTR kStartupMutex = L"Local\\ZipAppMFC.ExplorerRequest";

    // Parses the command line; returns false for /register and /unregister (handled here).
    bool ParseArguments(ShellRequest& request, bool& isRegister, bool& isUnregister)
    {
        isRegister = isUnregister = false;
        for (int i = 1; i < __argc; ++i)
        {
            const CString arg = __wargv[i];
            if (arg.CompareNoCase(L"/register") == 0 || arg.CompareNoCase(L"--register") == 0)
            {
                isRegister = true;
            }
            else if (arg.CompareNoCase(L"/unregister") == 0 || arg.CompareNoCase(L"--unregister") == 0)
            {
                isUnregister = true;
            }
            else if (arg.CompareNoCase(L"/add") == 0 || arg.CompareNoCase(L"--add") == 0)
            {
                request.kind = ShellRequest::Add;
            }
            else if (arg.CompareNoCase(L"/extract") == 0 || arg.CompareNoCase(L"--extract") == 0)
            {
                request.kind = ShellRequest::Extract;
            }
            else
            {
                request.paths.push_back(arg);
            }
        }

        // Files without a switch (e.g. dropped onto the program's icon) are added.
        if (request.kind == ShellRequest::None && !request.paths.empty())
        {
            request.kind = ShellRequest::Add;
        }

        return !isRegister && !isUnregister;
    }

    HWND FindMainWindow()
    {
        HWND found = nullptr;
        EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL
        {
            if (GetPropW(hwnd, CZipAppMFCDlg::kWindowProp) != nullptr)
            {
                *reinterpret_cast<HWND*>(lParam) = hwnd;
                return FALSE;
            }

            return TRUE;
        }, reinterpret_cast<LPARAM>(&found));

        return found;
    }

    // Sends the request to an open window; false if there is none or it did not answer.
    bool ForwardToMainWindow(const ShellRequest& request)
    {
        const HWND hwnd = FindMainWindow();
        if (!hwnd)
        {
            return false;
        }

        std::wstring data;
        for (const CString& path : request.paths)
        {
            data.append(path.GetString(), path.GetLength());
            data.push_back(L'\0');
        }

        // This process was started by the user's click, so it may bring that window forward.
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        AllowSetForegroundWindow(pid);

        COPYDATASTRUCT cds = {};
        cds.dwData = static_cast<ULONG_PTR>(request.kind);
        cds.cbData = static_cast<DWORD>(data.size() * sizeof(wchar_t));
        cds.lpData = &data[0];
        DWORD_PTR result = 0;
        return SendMessageTimeoutW(hwnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds),
            SMTO_ABORTIFHUNG, 10000, &result) != 0 && result == TRUE;
    }
}

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

    ShellRequest request;
    bool isRegister = false, isUnregister = false;
    if (!ParseArguments(request, isRegister, isUnregister))
    {
        // For installers and scripts: no window, only the exit code.
        CString error;
        const bool ok = isUnregister ? shell::Unregister() : shell::Register(error);
        m_exitCode = ok ? 0 : 1;
        return FALSE;
    }

    if (request.kind != ShellRequest::None)
    {
        // Wait for a window another selected file may be opening right now, then hand over.
        m_startupMutex = CreateMutexW(nullptr, FALSE, kStartupMutex);
        if (m_startupMutex)
        {
            WaitForSingleObject(m_startupMutex, 15000);   // WAIT_ABANDONED is fine too
        }

        if (ForwardToMainWindow(request))
        {
            MainWindowReady();
            return FALSE;
        }
    }

    CZipAppMFCDlg dlg;
    dlg.SetStartupRequest(request);
    m_pMainWnd = &dlg;
    dlg.DoModal();

    // In case the window failed to open: never keep other instances waiting.
    MainWindowReady();

    // Dialog closed: returning FALSE ends the application instead of starting a message pump.
    return FALSE;
}

void CZipAppMFCApp::MainWindowReady()
{
    if (m_startupMutex)
    {
        ReleaseMutex(m_startupMutex);
        CloseHandle(m_startupMutex);
        m_startupMutex = nullptr;
    }
}

int CZipAppMFCApp::ExitInstance()
{
    CWinApp::ExitInstance();
    return m_exitCode;
}
