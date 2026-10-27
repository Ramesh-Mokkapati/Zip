// CreatePage.h : the "Create archive" tab. Collects files, an archive format, an output path
// and (for ZIP) a password, then runs ZipEngine::CreateArchive on a worker thread so the
// window stays responsive. Hosted as a child dialog by CZipAppMFCDlg.
#pragma once

#include "../ZipEngineLib/ZipEngine.h"

class CCreatePage : public CDialogEx
{
public:
    explicit CCreatePage(CWnd* pParent = nullptr);

    enum { IDD = IDD_PAGE_CREATE };

    // Adds files to the list, skipping duplicates and folders. Returns how many were added.
    // Also used for files dragged onto the window while this tab is shown.
    int AddFiles(const std::vector<CString>& paths);

    // True while an archive is being written; RequestCancel asks the worker to stop.
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

    afx_msg void OnBnClickedBtnBrowse();
    afx_msg void OnBnClickedBtnRemove();
    afx_msg void OnBnClickedBtnClear();
    afx_msg void OnBnClickedBtnOutput();
    afx_msg void OnBnClickedBtnZip();
    afx_msg void OnBnClickedBtnStop();
    afx_msg void OnLbnSelchangeListFiles();
    afx_msg void OnCbnSelchangeComboFormat();
    afx_msg void OnDestroy();
    afx_msg LRESULT OnZipProgress(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnZipDone(WPARAM wParam, LPARAM lParam);
    DECLARE_MESSAGE_MAP()

private:
    void SuggestOutputPath();
    void UpdateControls();

    // Format currently chosen in the combo box.
    ZipEngine::ArchiveFormat CurrentFormat() const;
    void SelectFormat(ZipEngine::ArchiveFormat format);

    // Replaces the output path's archive extension (if any) with the chosen format's.
    void ApplyFormatExtension();
    static UINT __cdecl ZipWorker(LPVOID pParam);

    CListBox      m_listFiles;
    CComboBox     m_comboFormat;
    CProgressCtrl m_progress;

    // Worker thread state. m_cancel is set by the UI thread and polled by the worker.
    CWinThread* m_pWorker = nullptr;
    std::atomic<bool> m_cancel{ false };
    CString           m_runOutput;      // output path of the running / last job
    size_t            m_runFileCount = 0;
    bool              m_runEncrypted = false;
    bool              m_runLegacy = false;      // ZipCrypto instead of AES-256
    ZipEngine::ArchiveFormat m_runFormat = ZipEngine::ArchiveFormat::Zip;
};
