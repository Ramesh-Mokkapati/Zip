// ExtractPage.h : the "Extract archive" tab. Shows what an archive contains, then extracts it
// with ZipEngine::ExtractArchive on a worker thread (ZIP with or without a password, and every
// TAR variant). Hosted as a child dialog by CZipAppMFCDlg.
#pragma once

#include "../ZipEngineLib/ZipEngine.h"

class CExtractPage : public CDialogEx
{
public:
    explicit CExtractPage(CWnd* pParent = nullptr);

    enum { IDD = IDD_PAGE_EXTRACT };

    // Loads an archive (also used for a file dragged onto the window while this tab is shown).
    // fromExplorer: suggest a folder for it even if one was typed before, and put the focus on
    // the password box (encrypted) or the Extract button.
    void SetArchive(const CString& path, bool fromExplorer = false);

    bool IsBusy() const
    {
        return m_pWorker != nullptr;
    }

    void RequestCancel()
    {
        m_cancel = true;
    }

protected:
    void DoDataExchange(CDataExchange* pDX) override;
    BOOL OnInitDialog() override;

    // A child page must not end itself: Enter/Escape are handled by the main window.
    void OnOK() override {}
    void OnCancel() override {}

    afx_msg void OnBnClickedBtnArchive();
    afx_msg void OnBnClickedBtnDest();
    afx_msg void OnBnClickedBtnExtract();
    afx_msg void OnBnClickedBtnStop();
    afx_msg void OnEnChangeArchive();
    afx_msg void OnEnChangeDest();
    afx_msg void OnTimer(UINT_PTR nIDEvent);
    afx_msg void OnDestroy();
    afx_msg LRESULT OnExtractProgress(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnExtractDone(WPARAM wParam, LPARAM lParam);
    DECLARE_MESSAGE_MAP()

private:
    // Reads the archive's entry list and fills the contents view (no extraction).
    void LoadArchive();
    void UpdateControls();
    static UINT __cdecl ExtractWorker(LPVOID pParam);

    CListCtrl     m_listContents;
    CProgressCtrl m_progress;

    CString m_loadedArchive;          // the archive currently shown in the list
    bool    m_loadedOk = false;
    bool    m_anyEncrypted = false;
    bool    m_destIsSuggested = true; // false once the user types or picks a folder
    bool    m_settingDest = false;    // true while the program itself changes the folder box

    // Worker thread state.
    CWinThread* m_pWorker = nullptr;
    std::atomic<bool> m_cancel{ false };
    CString           m_runDest;
};
