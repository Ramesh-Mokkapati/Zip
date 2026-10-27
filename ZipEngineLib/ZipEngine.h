/// @file ZipEngine.h
/// @brief Creates ZIP archives (optionally AES-256 encrypted, via libzip) and TAR archives
///        (.tar, .tar.gz, .tar.bz2, .tar.xz, via libarchive), and lists archive contents.
///
/// All paths are **UTF-8** std::strings, so any file name works (convert from MFC/Win32
/// wide strings with CW2A(str, CP_UTF8)). Passwords are limited to printable ASCII: ZIP has
/// no standard password encoding, and 7-Zip interprets non-ASCII ZIP passwords in the local
/// ANSI code page, so only ASCII passwords open reliably on every computer.
///
/// ZIP archives encrypted with a password use AES-256 by default, which 7-Zip, WinRAR and
/// WinZip open. Windows Explorer's built-in ZIP support cannot open AES-encrypted archives; it
/// only knows the legacy "ZipCrypto" scheme. ZipCrypto can be chosen (ZipEncryption::ZipCrypto)
/// for archives that must open in Explorer, but it is weak: it can be broken in minutes or
/// hours with free tools, and it does not hide file names or sizes (neither does AES in ZIP).
/// TAR has no encryption at all; TAR archives are never password-protected.
///
/// Every writer stores each input under its own file name (no folders), validates all inputs
/// before writing, and never leaves a partial archive: on failure or cancellation nothing is
/// written and an existing file at the output path is left unchanged.
///
/// ExtractArchive unpacks ZIP (including AES-256 and legacy ZipCrypto encrypted entries) and
/// every TAR variant. It treats archive contents as untrusted: entries that would escape the
/// destination folder ("Zip Slip"), absolute paths, Windows device names and alternate data
/// streams are refused before anything is written; links and device entries are skipped.

#pragma once
#include <functional>
#include <string>
#include <vector>
#include <memory>

namespace ZipEngine
{
    /// Result of an archive operation. Anything other than Success leaves no new archive behind
    /// (an existing file at the output path is left unchanged).
    enum class Status
    {
        Success,            ///< The archive was written.
        FileNotFound,       ///< An input file does not exist or is a folder.
        ArchiveCreateError, ///< The output file could not be created or written.
        EncryptionError,    ///< AES-256 is not available in the libzip build, or could not be applied.
        UnknownError,       ///< Any other library failure; see the error message.
        InvalidArgument,    ///< Empty output path or file list, the output is also an input, the password is not
        ///< printable ASCII, or a password was given for a format that cannot be encrypted.
        DuplicateEntryName, ///< Two inputs have the same file name (entries are stored without folders).
        FileReadError,      ///< An input file exists but could not be read (locked, access denied).
        Cancelled,          ///< The progress callback asked to stop.

        // Extraction only
        PasswordRequired,   ///< The ZIP has encrypted entries and no password was given.
        WrongPassword,      ///< The password does not decrypt the archive (or the encrypted data is corrupt).
        UnsafeEntry,        ///< An entry would be written outside the destination or has a name Windows cannot use safely.
        FileExists          ///< A file to be extracted already exists and overwriting was not allowed.
    };

    /// Called during compression with the overall progress (0.0 to 1.0).
    /// Return false to cancel. Called on the thread that creates the archive.
    typedef std::function<bool(double progress)> ProgressCallback;

    /// Short English description of a Status value (for logs and message boxes).
    const char* StatusToString(Status status);

    // ---------------------------------------------------------------------------------------
    // Archive formats

    /// The archive types this library can create.
    enum class ArchiveFormat
    {
        Zip,      ///< .zip  - Deflate, optional AES-256 encryption (libzip)
        Tar,      ///< .tar  - uncompressed POSIX (pax) tar (libarchive)
        TarGz,    ///< .tar.gz / .tgz   - tar compressed with gzip
        TarBz2,   ///< .tar.bz2 / .tbz2 - tar compressed with bzip2
        TarXz     ///< .tar.xz / .txz   - tar compressed with xz (LZMA2), usually the smallest
    };

    /// How a password-protected ZIP is encrypted.
    enum class ZipEncryption
    {
        Aes256,      ///< WinZip AE-2, 256-bit AES: strong. Not readable by Windows Explorer.
        ZipCrypto,   ///< Traditional PKWARE encryption: opens in Windows Explorer and almost any
        ///< ZIP tool, but is weak (known-plaintext attacks break it quickly).
    };

    /// Display name, e.g. "ZIP", "TAR.GZ".
    const char* FormatName(ArchiveFormat format);

    /// Preferred file extension including the dot, e.g. ".zip", ".tar.gz".
    const char* FormatExtension(ArchiveFormat format);

    /// True only for ZIP: TAR has no encryption.
    bool FormatSupportsEncryption(ArchiveFormat format);

    /// Detects the format from a file name's extension (case-insensitive): .zip, .tar, .tar.gz,
    /// .tgz, .tar.bz2, .tbz2, .tbz, .tar.xz, .txz. Returns false for anything else.
    bool FormatFromPath(const std::string& path, ArchiveFormat& format);

    /// Parses a format name as typed on a command line: "zip", "tar", "tar.gz"/"tgz"/"gz",
    /// "tar.bz2"/"tbz2"/"bz2", "tar.xz"/"txz"/"xz" (case-insensitive). Returns false if unknown.
    bool FormatFromName(const std::string& name, ArchiveFormat& format);

    // ---------------------------------------------------------------------------------------
    // ZIP (runbook API)

    class ZipCompressor
    {
    public:
        ZipCompressor() = default;
        ~ZipCompressor() = default;

        // C++11: Disable copy construction and assignment
        ZipCompressor(const ZipCompressor&) = delete;
        ZipCompressor& operator=(const ZipCompressor&) = delete;

        // Move constructor and assignment
        ZipCompressor(ZipCompressor&&) noexcept = default;
        ZipCompressor& operator=(ZipCompressor&&) noexcept = default;

        /// Creates a zip archive containing the specified files, with optional AES-256 encryption.
        ///
        /// Each file is stored under its own file name (no folder), compressed with Deflate.
        /// The archive is built in a temporary file and only replaces @p outputZipPath once it is
        /// complete, so a failure or cancellation never leaves a half-written ZIP.
        ///
        /// @param outputZipPath UTF-8 path of the .zip to create; an existing file is replaced.
        /// @param filesToAdd    UTF-8 paths of the files to add (at least one; folders are not accepted).
        /// @param password      Empty = no encryption. Otherwise every entry is encrypted with
        ///                      @p encryption. Must be printable ASCII (letters, digits, space,
        ///                      punctuation); anything else returns Status::InvalidArgument.
        /// @param progress      Optional progress/cancel callback (see ProgressCallback).
        /// @param encryption    AES-256 (default; 7-Zip/WinRAR/WinZip) or legacy ZipCrypto (also
        ///                      Windows Explorer, but weak). Ignored without a password.
        /// @return Status::Success, or the reason for failure (details in GetLastErrorMessage()).
        Status CreateEncryptedZip(
            const std::string& outputZipPath,
            const std::vector<std::string>& filesToAdd,
            const std::string& password = "",
            ProgressCallback progress = nullptr,
            ZipEncryption encryption = ZipEncryption::Aes256
        );

        /// Human-readable description of the last failure (UTF-8), empty after a success.
        /// Names the file involved and includes libzip's own error text where available.
        const std::string& GetLastErrorMessage() const { return m_lastError; }

    private:
        Status Fail(Status status, const std::string& message);

        std::string m_lastError;
    };

    // ---------------------------------------------------------------------------------------
    // TAR

    class TarCompressor
    {
    public:
        TarCompressor() = default;
        ~TarCompressor() = default;
        TarCompressor(const TarCompressor&) = delete;
        TarCompressor& operator=(const TarCompressor&) = delete;
        TarCompressor(TarCompressor&&) noexcept = default;
        TarCompressor& operator=(TarCompressor&&) noexcept = default;

        /// Creates a POSIX (pax) tar archive, optionally gzip/bzip2/xz compressed.
        ///
        /// Each file is stored under its own file name with its modification time; non-ASCII
        /// names are stored as UTF-8 (pax extended headers), which GNU tar, bsdtar (Windows'
        /// tar.exe) and 7-Zip all read. The archive is written to a temporary file next to the
        /// output and renamed over it only when complete.
        ///
        /// @param outputTarPath UTF-8 path of the archive to create; an existing file is replaced.
        /// @param filesToAdd    UTF-8 paths of the files to add (at least one; folders are not accepted).
        /// @param format        Tar, TarGz, TarBz2 or TarXz (Zip returns Status::InvalidArgument).
        /// @param progress      Optional progress/cancel callback.
        /// @return Status::Success, or the reason for failure (details in GetLastErrorMessage()).
        Status CreateTar(
            const std::string& outputTarPath,
            const std::vector<std::string>& filesToAdd,
            ArchiveFormat format = ArchiveFormat::TarGz,
            ProgressCallback progress = nullptr
        );

        /// Human-readable description of the last failure (UTF-8), empty after a success.
        const std::string& GetLastErrorMessage() const
        {
            return m_lastError;
        }

    private:
        Status Fail(Status status, const std::string& message);

        std::string m_lastError;
    };

    // ---------------------------------------------------------------------------------------
    // One entry point for every format, and listing

    /// Creates an archive of any supported format; used by the MFC app and zipcli.
    /// @param password Only for ArchiveFormat::Zip. A non-empty password with a TAR format
    ///                 returns Status::InvalidArgument (TAR cannot be encrypted).
    /// @param errorMessage Receives the failure description (empty on success).
    Status CreateArchive(
        ArchiveFormat format,
        const std::string& outputPath,
        const std::vector<std::string>& filesToAdd,
        const std::string& password,
        ProgressCallback progress,
        std::string& errorMessage
    );

    /// As above, choosing how a password-protected ZIP is encrypted (see ZipEncryption).
    Status CreateArchive(
        ArchiveFormat format,
        const std::string& outputPath,
        const std::vector<std::string>& filesToAdd,
        const std::string& password,
        ZipEncryption encryption,
        ProgressCallback progress,
        std::string& errorMessage
    );

    /// One entry as reported by ListArchive.
    struct ArchiveEntryInfo
    {
        std::string        name;        ///< UTF-8 path inside the archive
        unsigned long long size = 0;    ///< uncompressed size in bytes (0 if the format does not record it)
        bool               encrypted = false;
        bool               isDirectory = false;
    };

    /// Lists the entries of a ZIP or TAR archive (any of the formats above, detected from the
    /// content, not the extension). Encrypted ZIP entries are listed without a password, since
    /// ZIP stores names unencrypted.
    Status ListArchive(const std::string& archivePath, std::vector<ArchiveEntryInfo>& entries, std::string& errorMessage);

    // ---------------------------------------------------------------------------------------
    // Extraction

    /// What ExtractArchive did.
    struct ExtractSummary
    {
        unsigned                 files = 0;     ///< regular files written
        unsigned                 folders = 0;   ///< folders created (explicit entries and parents)
        unsigned long long       bytes = 0;     ///< bytes written
        std::vector<std::string> skipped;       ///< entries not extracted on purpose (links, devices), UTF-8
    };

    /// Extracts a ZIP or TAR archive (format detected from the content) into a folder.
    ///
    /// Safety: the whole archive is checked before anything is written. These make the call fail
    /// with Status::UnsafeEntry and write nothing:
    ///  - names that would leave the destination ("../x", "a/../../x", "/x", "C:\x", "\\server\x")
    ///  - Windows device names (CON, NUL, COM1, LPT1, ... with any extension), ':' (alternate data
    ///    streams), characters Windows forbids, and names ending in a dot or space
    ///  - two entries that would write the same file
    /// Symbolic links, hard links and device entries (TAR, or ZIP from Unix) are skipped and listed
    /// in summary.skipped, so an archive cannot plant a link that redirects later writes.
    ///
    /// Each file is written to a temporary name and renamed into place only when complete. If the
    /// extraction fails or is cancelled, the files and folders it created are removed again.
    ///
    /// @param archivePath UTF-8 path of the archive.
    /// @param destFolder  UTF-8 folder to extract into; created if missing.
    /// @param password    For encrypted ZIP entries (AES or ZipCrypto). Ignored for TAR.
    /// @param overwrite   false: fail with Status::FileExists if any target file exists (nothing is
    ///                    written). true: replace existing files.
    /// @param progress    Optional progress/cancel callback (by bytes written).
    /// @param summary     Receives counts and skipped entries.
    /// @param errorMessage Receives the failure description (empty on success).
    Status ExtractArchive(
        const std::string& archivePath,
        const std::string& destFolder,
        const std::string& password,
        bool overwrite,
        ProgressCallback progress,
        ExtractSummary& summary,
        std::string& errorMessage
    );

} // namespace ZipEngine
