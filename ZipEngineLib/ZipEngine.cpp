// ZipEngine.cpp
// ZIP creation with libzip. The work happens in three phases:
//   1. Validate every input up front (detail::ValidateInputs: exists, readable, unique entry
//      names) so the common mistakes are reported with the offending file name before
//      anything is written.
//   2. Describe the archive to libzip: one zip_source per file, plus encryption settings.
//      Nothing is read or compressed yet.
//   3. zip_close() reads, compresses and encrypts everything into a temporary file and
//      renames it over the output only when complete. All failures before or during this
//      step call zip_discard(), which throws the half-built archive away.
//
// libzip on Windows interprets char* file names as UTF-8 (since libzip 1.7), which is why
// this library uses UTF-8 strings everywhere.

#include "ZipEngine.h"
#include "ArchiveCommon.h"

#include <zip.h>

namespace ZipEngine
{
    namespace
    {
        // libzip's error text for an error code (e.g. from zip_open).
        std::string ZipErrorText(int code)
        {
            zip_error_t error;
            zip_error_init_with_code(&error, code);
            const std::string text = zip_error_strerror(&error);
            zip_error_fini(&error);
            return text;
        }

        // State shared with libzip's C callbacks during zip_close().
        struct CallbackState
        {
            ProgressCallback progress;
            bool cancelRequested = false;
        };

        // libzip reports progress (0..1); forward it and remember whether the caller wants to stop.
        void ProgressThunk(zip_t*, double fraction, void* userData)
        {
            CallbackState* state = static_cast<CallbackState*>(userData);
            if (state->progress && !state->progress(fraction))
            {
                state->cancelRequested = true;
            }
        }

        // libzip polls this while writing; a non-zero return aborts zip_close() with ZIP_ER_CANCELLED.
        int CancelThunk(zip_t*, void* userData)
        {
            return static_cast<CallbackState*>(userData)->cancelRequested ? 1 : 0;
        }

    } // namespace

    const char* StatusToString(Status status)
    {
        switch (status)
        {

        case Status::Success:
            return "Success";

        case Status::FileNotFound:
            return "Input file not found";

        case Status::ArchiveCreateError:
            return "Could not create the archive";

        case Status::EncryptionError:
            return "Encryption failed";

        case Status::UnknownError:
            return "Unexpected error";

        case Status::InvalidArgument:
            return "Invalid input";

        case Status::DuplicateEntryName:
            return "Two files have the same name";

        case Status::FileReadError:
            return "Input file could not be read";

        case Status::Cancelled:
            return "Cancelled";

        case Status::PasswordRequired:
            return "A password is required";

        case Status::WrongPassword:
            return "Wrong password";

        case Status::UnsafeEntry:
            return "Unsafe entry in the archive";

        case Status::FileExists:
            return "A file already exists";
        }

        return "Unknown status";
    }

    Status ZipCompressor::Fail(Status status, const std::string& message)
    {
        m_lastError = message;
        return status;
    }

    Status ZipCompressor::CreateEncryptedZip(
        const std::string& outputZipPath,
        const std::vector<std::string>& filesToAdd,
        const std::string& password,
        ProgressCallback progress,
        ZipEncryption encryption)
    {
        m_lastError.clear();
        const bool aes = encryption == ZipEncryption::Aes256;
        const std::string methodName = aes ? "AES-256" : "ZipCrypto";

        // ---- Phase 1: validate ---------------------------------------------------------
        // ZIP has no standard password encoding: 7-Zip reads ZIP passwords in the computer's ANSI
        // code page (and refuses to create ZIPs with non-ASCII passwords), so a password with
        // accented or non-Latin characters may be impossible to type correctly elsewhere.
        // Printable ASCII is encoded identically everywhere, so it is the only safe choice.
        for (const char c : password)
        {
            if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E)
            {
                return Fail(Status::InvalidArgument,
                    "The password may only contain English letters, digits, spaces and punctuation, "
                    "so that 7-Zip, WinRAR and WinZip on any computer can open the archive.");
            }
        }

        detail::ValidatedInputs inputs;
        std::string error;
        const Status valid = detail::ValidateInputs(outputZipPath, filesToAdd, inputs, error);
        if (valid != Status::Success)
        {
            return Fail(valid, error);
        }

        // ---- Phase 2: describe the archive ----------------------------------------------
        // ZIP_TRUNCATE: start empty even if the file exists. Nothing touches the disk until zip_close.
        int err = 0;
        zip_t* archive = zip_open(outputZipPath.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
        if (!archive)
        {
            return Fail(Status::ArchiveCreateError, "Cannot create " + outputZipPath + ": " + ZipErrorText(err));
        }

        for (std::size_t i = 0; i < filesToAdd.size(); ++i)
        {
            const std::string& filePath = filesToAdd[i];

            // -1 = whole file. The file is opened and read later, during zip_close().
            zip_source_t* source = zip_source_file(archive, filePath.c_str(), 0, -1);
            if (!source)
            {
                const std::string why = zip_strerror(archive);
                zip_discard(archive);
                return Fail(Status::FileNotFound, "Cannot add " + filePath + ": " + why);
            }

            // ZIP_FL_ENC_UTF_8 marks the name as UTF-8 so non-ASCII names extract correctly.
            zip_int64_t index = zip_file_add(archive, inputs.entryNames[i].c_str(), source, ZIP_FL_ENC_UTF_8);
            if (index < 0)
            {
                const std::string why = zip_strerror(archive);
                zip_source_free(source);   // only freed by us when zip_file_add fails
                zip_discard(archive);
                return Fail(Status::ArchiveCreateError, "Cannot add " + filePath + ": " + why);
            }

            // Apply encryption if password provided. ZIP_EM_AES_256 = WinZip AE-2 with a 256-bit key
            // derived from the password; ZIP_EM_TRAD_PKWARE = legacy ZipCrypto. libzip copies the
            // password, so the caller may wipe it after.
            if (!password.empty())
            {
                const zip_uint16_t method = aes ? ZIP_EM_AES_256 : ZIP_EM_TRAD_PKWARE;
                if (zip_file_set_encryption(archive, static_cast<zip_uint64_t>(index), method, password.c_str()) < 0)
                {
                    const std::string why = zip_strerror(archive);
                    zip_discard(archive);
                    return Fail(Status::EncryptionError, methodName + " encryption is not available: " + why);
                }
            }
        }

        // ---- Phase 3: write -----------------------------------------------------------------
        CallbackState state;
        state.progress = progress;
        if (progress)
        {
            // Report roughly every 1%; the cancel callback lets a UI stop a long run.
            zip_register_progress_callback_with_state(archive, 0.01, ProgressThunk, nullptr, &state);
            zip_register_cancel_callback_with_state(archive, CancelThunk, nullptr, &state);
        }

        if (zip_close(archive) < 0)
        {
            // On failure zip_close leaves the archive open; read the error, then discard it
            // (which also deletes libzip's temporary file).
            zip_error_t* zerror = zip_get_error(archive);
            const int code = zip_error_code_zip(zerror);
            const std::string why = zip_error_strerror(zerror);
            zip_discard(archive);

            switch (code)
            {

            case ZIP_ER_CANCELLED:
                return Fail(Status::Cancelled, "Cancelled; no archive was written.");

            case ZIP_ER_OPEN:
            case ZIP_ER_READ:
            case ZIP_ER_NOENT:
                return Fail(Status::FileReadError, "An input file could not be read: " + why);

            case ZIP_ER_TMPOPEN:
            case ZIP_ER_WRITE:
            case ZIP_ER_RENAME:
            case ZIP_ER_REMOVE:
                return Fail(Status::ArchiveCreateError, "Cannot write " + outputZipPath + ": " + why);

            case ZIP_ER_ENCRNOTSUPP:
                return Fail(Status::EncryptionError, methodName + " encryption is not available: " + why);

            default:
                return Fail(Status::UnknownError, "Creating the archive failed: " + why);
            }
        }

        if (progress)
        {
            progress(1.0);
        }

        return Status::Success;
    }

} // namespace ZipEngine
