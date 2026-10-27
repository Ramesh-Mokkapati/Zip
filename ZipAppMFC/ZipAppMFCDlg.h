// ZipAppMFCDlg.h : main window. Collects files, an archive format, an output path and (for
// ZIP) a password, then runs ZipEngine::CreateArchive on a worker thread so the window stays
// responsive.
#pragma once

#include "../ZipEngineLib/ZipEngine.h"

class CZipAppMFCDlg : public CDialogEx
{
public:
    explicit CZipAppMFCDlg(CWnd* pParent = nullptr);

    enum { IDD = IDD_ZIPAPPMFC_DIALOG };

protected:
    void DoDataExchange(CDataExchange* pDX) override;
    BOOL OnInitDialog() override;
    void OnOK() override {}       // Enter must not close the window (Create Zip is the default button)
    void OnCancel() override;     // Close button / Escape / title-bar X

    afx_msg void OnBnClickedBtnBrowse();
    afx_msg void OnBnClickedBtnRemove();
    afx_msg void OnBnClickedBtnClear();
    afx_msg void OnBnClickedBtnOutput();
    afx_msg void OnBnClickedBtnZip();
    afx_msg void OnBnClickedBtnStop();
    afx_msg void OnLbnSelchangeListFiles();
    afx_msg void OnCbnSelchangeComboFormat();
    afx_msg void OnDropFiles(HDROP hDropInfo);
    afx_msg void OnDestroy();
    afx_msg LRESULT OnZipProgress(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnZipDone(WPARAM wParam, LPARAM lParam);
    DECLARE_MESSAGE_MAP()

private:
    // Adds files to the list, skipping duplicates and folders. Returns how many were added.
    int AddFiles(const std::vector<CString>& paths);
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
    HICON         m_hIcon = nullptr;

    // Worker thread state. m_cancel is set by the UI thread and polled by the worker.
    CWinThread* m_pWorker = nullptr;
    std::atomic<bool> m_cancel{ false };
    CString           m_runOutput;      // output path of the running / last job
    size_t            m_runFileCount = 0;
    bool              m_runEncrypted = false;
    ZipEngine::ArchiveFormat m_runFormat = ZipEngine::ArchiveFormat::Zip;
};
