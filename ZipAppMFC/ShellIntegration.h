// ShellIntegration.h : the app's entries in Explorer's right-click menu, for the current user
// only (HKEY_CURRENT_USER\Software\Classes, so no administrator rights are needed):
//
//   any file             "Add to archive (ZIP / TAR)..."      ZipAppMFC.exe /add "<file>"
//   .zip, .tar, .tgz ... "Extract with ZIP / TAR Archive Tool..." ZipAppMFC.exe /extract "<archive>"
//
// They are classic menu entries; on Windows 11 they appear under "Show more options".
#pragma once

namespace shell
{
    enum class State
    {
        NotRegistered,
        Registered,            // registered for this ZipAppMFC.exe
        RegisteredElsewhere,   // registered for a copy of the app in another folder
    };

    State GetState();

    // Adds the menu entries for this executable (replacing entries for another copy). On
    // failure nothing is left behind and error describes the problem.
    bool Register(CString& error);

    // Removes the menu entries; true if none are left (also when there were none).
    bool Unregister();
}
