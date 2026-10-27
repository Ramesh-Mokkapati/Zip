// pch.h : precompiled header for ZipAppMFC (MFC headers and the standard library).
#pragma once

#ifndef VC_EXTRALEAN
#define VC_EXTRALEAN            // exclude rarely-used parts of the Windows headers
#endif

#include <sdkddkver.h>          // _WIN32_WINNT comes from the project settings

#define _ATL_CSTRING_EXPLICIT_CONSTRUCTORS
#define _AFX_ALL_WARNINGS

#include <afxwin.h>             // MFC core
#include <afxext.h>
#include <afxcmn.h>             // CProgressCtrl
#include <afxdialogex.h>

#include <atomic>
#include <string>
#include <vector>

// Common Controls v6 for themed buttons, progress bar and list box.
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
