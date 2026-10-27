// ArchiveFormat.cpp : format names/extensions, the one-call CreateArchive, and ListArchive.

#include "ZipEngine.h"
#include "ArchiveCommon.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cctype>
#include <memory>

namespace ZipEngine
{
    namespace
    {
        std::string Lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
            return s;
        }

        bool EndsWith(const std::string& s, const std::string& suffix)
        {
            return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        struct ArchiveReadDeleter
        {
            void operator()(archive* a) const { archive_read_free(a); }
        };

    } // namespace

    const char* FormatName(ArchiveFormat format)
    {
        switch (format)
        {

        case ArchiveFormat::Zip:
            return "ZIP";

        case ArchiveFormat::Tar:
            return "TAR";

        case ArchiveFormat::TarGz:
            return "TAR.GZ";

        case ArchiveFormat::TarBz2:
            return "TAR.BZ2";

        case ArchiveFormat::TarXz:
            return "TAR.XZ";
        }

        return "?";
    }

    const char* FormatExtension(ArchiveFormat format)
    {
        switch (format)
        {

        case ArchiveFormat::Zip:
            return ".zip";

        case ArchiveFormat::Tar:
            return ".tar";

        case ArchiveFormat::TarGz:
            return ".tar.gz";

        case ArchiveFormat::TarBz2:
            return ".tar.bz2";

        case ArchiveFormat::TarXz:
            return ".tar.xz";
        }

        return "";
    }

    bool FormatSupportsEncryption(ArchiveFormat format)
    {
        return format == ArchiveFormat::Zip;
    }

    bool FormatFromPath(const std::string& path, ArchiveFormat& format)
    {
        // Longest suffixes first, so ".tar.gz" is not mistaken for something ending in ".gz" alone.
        const std::string p = Lower(path);
        if (EndsWith(p, ".tar.gz") || EndsWith(p, ".tgz"))
        {
            format = ArchiveFormat::TarGz;
            return true;
        }

        if (EndsWith(p, ".tar.bz2") || EndsWith(p, ".tbz2") || EndsWith(p, ".tbz"))
        {
            format = ArchiveFormat::TarBz2;
            return true;
        }

        if (EndsWith(p, ".tar.xz") || EndsWith(p, ".txz"))
        {
            format = ArchiveFormat::TarXz;
            return true;
        }

        if (EndsWith(p, ".tar"))
        {
            format = ArchiveFormat::Tar;
            return true;
        }

        if (EndsWith(p, ".zip"))
        {
            format = ArchiveFormat::Zip;
            return true;
        }

        return false;
    }

    bool FormatFromName(const std::string& name, ArchiveFormat& format)
    {
        const std::string n = Lower(name);
        if (n == "zip")
        {
            format = ArchiveFormat::Zip;
            return true;
        }

        if (n == "tar")
        {
            format = ArchiveFormat::Tar;
            return true;
        }

        if (n == "tar.gz" || n == "tgz" || n == "gz")
        {
            format = ArchiveFormat::TarGz;
            return true;
        }

        if (n == "tar.bz2" || n == "tbz2" || n == "bz2")
        {
            format = ArchiveFormat::TarBz2;
            return true;
        }

        if (n == "tar.xz" || n == "txz" || n == "xz")
        {
            format = ArchiveFormat::TarXz;
            return true;
        }

        return false;
    }

    Status CreateArchive(
        ArchiveFormat format,
        const std::string& outputPath,
        const std::vector<std::string>& filesToAdd,
        const std::string& password,
        ProgressCallback progress,
        std::string& errorMessage)
    {
        return CreateArchive(format, outputPath, filesToAdd, password, ZipEncryption::Aes256, progress, errorMessage);
    }

    Status CreateArchive(
        ArchiveFormat format,
        const std::string& outputPath,
        const std::vector<std::string>& filesToAdd,
        const std::string& password,
        ZipEncryption encryption,
        ProgressCallback progress,
        std::string& errorMessage)
    {
        errorMessage.clear();

        if (format == ArchiveFormat::Zip)
        {
            ZipCompressor zip;
            const Status status = zip.CreateEncryptedZip(outputPath, filesToAdd, password, progress, encryption);
            errorMessage = zip.GetLastErrorMessage();
            return status;
        }

        // Silently ignoring a password would leave the user believing the archive is protected.
        if (!password.empty())
        {
            errorMessage = std::string(FormatName(format)) +
                " archives cannot be encrypted. Remove the password, or choose ZIP for a password-protected archive.";
            return Status::InvalidArgument;
        }

        TarCompressor tar;
        const Status status = tar.CreateTar(outputPath, filesToAdd, format, progress);
        errorMessage = tar.GetLastErrorMessage();
        return status;
    }

    Status ListArchive(const std::string& archivePath, std::vector<ArchiveEntryInfo>& entries, std::string& errorMessage)
    {
        entries.clear();
        errorMessage.clear();

        std::wstring wide;
        if (!detail::ToWide(archivePath, wide) || wide.empty())
        {
            errorMessage = "The archive path is empty or not valid UTF-8.";
            return Status::InvalidArgument;
        }

        // ZIP: read the central directory with libzip, the same reader ExtractArchive uses
        // (libarchive's streaming ZIP reader can stumble over encrypted entries).
        if (detail::IsZipFile(wide))
        {
            return detail::ListZip(archivePath, entries, errorMessage);
        }

        // TAR: detect the compression (gz/bz2/xz) from the data itself.
        std::unique_ptr<archive, ArchiveReadDeleter> a(archive_read_new());
        archive_read_support_filter_all(a.get());
        archive_read_support_format_zip(a.get());
        archive_read_support_format_tar(a.get());
        archive_read_support_format_gnutar(a.get());

        if (archive_read_open_filename_w(a.get(), wide.c_str(), 64 * 1024) != ARCHIVE_OK)
        {
            const char* why = archive_error_string(a.get());
            errorMessage = "Cannot open " + archivePath + ": " + (why ? why : "not a ZIP or TAR archive");
            return (GetFileAttributesW(wide.c_str()) == INVALID_FILE_ATTRIBUTES) ? Status::FileNotFound : Status::FileReadError;
        }

        archive_entry* entry = nullptr;
        int rc = ARCHIVE_OK;
        while ((rc = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK || rc == ARCHIVE_WARN)
        {
            ArchiveEntryInfo info;

            // Prefer the UTF-8 name (pax/zip UTF-8 flag); fall back to the wide name.
            const char* utf8 = archive_entry_pathname_utf8(entry);
            if (utf8)
            {
                info.name = utf8;
            }
            else if (const wchar_t* w = archive_entry_pathname_w(entry))
            {
                info.name = detail::ToUtf8(w);
            }

            info.size = archive_entry_size_is_set(entry) ? static_cast<unsigned long long>(archive_entry_size(entry)) : 0;
            info.encrypted = archive_entry_is_encrypted(entry) != 0;
            info.isDirectory = archive_entry_filetype(entry) == AE_IFDIR;
            entries.push_back(info);

            // Only names are needed; skip the data (works for encrypted ZIP entries too).
            archive_read_data_skip(a.get());
        }

        if (rc != ARCHIVE_EOF)
        {
            const char* why = archive_error_string(a.get());
            errorMessage = "Error reading " + archivePath + ": " + (why ? why : "corrupt archive");
            return Status::FileReadError;
        }

        return Status::Success;
    }

} // namespace ZipEngine
