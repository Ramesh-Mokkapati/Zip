# ZIP / TAR Archive Tool (C++ / MFC / libzip / libarchive)

A Windows tool that packs files into an archive and extracts archives, including password-protected (AES-256) ZIP files that Windows Explorer can't open. It comes as a desktop app, with optional entries in Explorer's right-click menu, and as a command-line utility.

| Format | Extensions | Compression | Password |
|---|---|---|---|
| ZIP | `.zip` | Deflate | Optional: AES-256, or legacy ZipCrypto for Windows Explorer |
| TAR | `.tar` | None | No |
| TAR.GZ | `.tar.gz`, `.tgz` | gzip | No |
| TAR.BZ2 | `.tar.bz2`, `.tbz2` | bzip2 | No |
| TAR.XZ | `.tar.xz`, `.txz` | xz (LZMA2), usually the smallest | No |

It's built from `zip_creation_application_runbook.md` as a Visual Studio 2022 solution:

| Project | Type | Purpose |
|---|---|---|
| `ZipEngineLib` | Static library | Archive creation, listing and safe extraction: libzip for ZIP, libarchive for TAR |
| `ZipAppMFC` | MFC dialog app | The desktop app |
| `ZipCli` | Console app | `zipcli.exe`, the command-line utility |
| `ZipEngineTests` | Console app | Builds real archives and verifies them with libzip, libarchive, 7-Zip and Windows `tar.exe` |

## Desktop app

The window has two tabs. Files dragged onto it go to the tab that is shown.

**Create archive**

1. **Add files:** use **Add Files...** or drag them from Explorer. **Remove** and **Clear** edit the list. Folders aren't accepted, because entries are stored by file name only.
2. **Pick a format:** choose it under **Archive format**. The output file's extension follows automatically.
3. **Choose the output:** check or change **Save archive as**. A name is suggested next to the first file. If you type a name whose extension implies another format (e.g. `.tar.gz` with ZIP selected), the app asks which you meant.
4. **Set a password (ZIP only):** type it in both boxes, or leave them empty for no encryption. The password fields are disabled for TAR formats, because TAR has no encryption.
   - By default the files are encrypted with **AES-256**: strong, but Windows Explorer can't open it (this app, 7-Zip, WinRAR and WinZip can).
   - Tick **Use legacy encryption (ZipCrypto)** if the recipient will open the file with Windows Explorer. The text under the box explains the trade-off: ZipCrypto opens almost anywhere, but it's weak, and free tools can recover the files. Don't use it for sensitive data.
5. **Create:** click **Create Archive**. A progress bar shows the work, and **Stop** cancels it.

**Extract archive**

1. **Choose the archive:** use **Browse...**, paste a path, or drag a `.zip` or `.tar*` file onto the window. Its contents are listed straight away, with sizes and which entries are encrypted.
2. **Choose the folder:** a folder named after the archive, next to it, is suggested. It's created if needed.
3. **Password:** enabled only when the archive has encrypted entries. A wrong password is reported before anything is written.
4. **Extract:** click **Extract**. Existing files are never replaced unless **Replace existing files** is ticked. **Stop** cancels and removes the files extracted so far. When it's done, the app offers to open the folder.

**Explorer right-click menu**

Click **Add to Explorer menu** at the bottom of the window. It adds two entries for your user account only, so no administrator rights are needed:

| Right-click on | Entry | Opens |
|---|---|---|
| Any file (several can be selected) | **Add to archive (ZIP / TAR)...** | The Create tab, with the selected files listed |
| `.zip`, `.tar`, `.tgz`, `.tar.gz`, `.tar.bz2`, `.tar.xz` ... | **Extract with ZIP / TAR Archive Tool...** | The Extract tab, with the archive loaded and a folder suggested |

- **Windows 11:** the entries are under **Show more options** (or Shift+right-click). Putting them in the short top-level menu would need a packaged, signed COM extension.
- **One window:** Explorer starts the program once per selected file. Each copy hands its file to the window that is already open, so a multi-file selection lands in one list. Selecting more than 15 files makes Explorer ask before running the entry.
- **Removing them:** the same button reads **Remove from Explorer menu**. The entries point to the copy of `ZipAppMFC.exe` that added them, so if you move the program, add them again.
- **Scripts and installers:** use `ZipAppMFC.exe /register` and `ZipAppMFC.exe /unregister`. They show no window; exit code `0` means success.
- **Other command lines:** `ZipAppMFC.exe /add <file>...` and `ZipAppMFC.exe /extract <archive>` do what the menu entries do.

## Command-line utility

```bash
zipcli create -p secrets.zip report.docx budget.xlsx
```

```bash
zipcli create logs.tar.gz C:/Logs/*.log
```

```bash
zipcli list secrets.zip
```

```bash
zipcli extract secrets.zip -d C:/Restore
```

```bash
zipcli create -p --legacy for-explorer.zip notes.txt
```

Options for `create`:

| Option | Meaning |
|---|---|
| `-f, --format <fmt>` | `zip`, `tar`, `tar.gz`, `tar.bz2` or `tar.xz`. Overrides the format implied by the archive's extension. |
| `-e, --encryption <m>` | `aes256` (default) or `zipcrypto`, for a password-protected ZIP |
| `--legacy` | Same as `--encryption zipcrypto`: Windows Explorer can open the ZIP, but the encryption is weak. `zipcli` prints a reminder. |
| `-p, --password-prompt` | Ask for a password, hidden and asked twice (ZIP only) |
| `--password-env <VAR>` | Read the password from an environment variable. Best for scripts. |
| `--password <text>` | Password on the command line. Avoid: other programs can see it, and it's kept in shell history. |
| `--force` | Replace an existing archive. Without it, an existing file is never overwritten. |
| `-q, --quiet` | No progress bar or summary |

Options for `extract` (short form `x`):

| Option | Meaning |
|---|---|
| `-d, --dest <folder>` | Where to extract; created if needed. Default: a folder named after the archive (`secrets.zip` → `secrets`) in the current folder. |
| `-p, --password-prompt` | Ask for the password, hidden. For an encrypted archive it's also asked automatically when `zipcli` runs in a console; in a script, use `--password-env`. |
| `--password-env <VAR>`, `--password <text>` | As for `create` |
| `--force` | Replace files that already exist in the folder. Without it, nothing is extracted if any would be replaced. |
| `-q, --quiet` | No progress bar or summary |

- **Wildcards:** `*` and `?` in file arguments are expanded by `zipcli` itself; the Windows shell doesn't do it.
- **Ctrl+C:** cancels cleanly, leaving no partial archive; a cancelled extraction removes what it had written.
- **`list`:** works on any ZIP or TAR variant (the format is detected from the content) and marks encrypted entries.
- **Exit codes:** `0` success, `1` usage error, `2` failed, `3` cancelled.

## The engine API

```cpp
#include "ZipEngine.h"

// Any format, one call (used by the app and zipcli):
std::string error;
ZipEngine::Status status = ZipEngine::CreateArchive(
    ZipEngine::ArchiveFormat::TarXz, u8"C:\\Temp\\logs.tar.xz",
    { u8"C:\\Temp\\a.log", u8"C:\\Temp\\b.log" },
    "",                                              // password: ZIP only
    [](double progress) { return true; },            // optional: 0..1, return false to cancel
    error);

// The runbook's ZIP API is unchanged:
ZipEngine::ZipCompressor compressor;
status = compressor.CreateEncryptedZip(u8"C:\\Temp\\Archive.zip", { u8"C:\\Temp\\test1.txt" }, "password");
```

Also available:
- `TarCompressor::CreateTar(output, files, format, progress)`
- `ListArchive(path, entries, error)`
- `ExtractArchive(archive, destFolder, password, overwrite, progress, summary, error)`, for any ZIP (plain, AES or legacy ZipCrypto, from any tool) or TAR variant
- `FormatFromPath`, `FormatFromName`, `FormatExtension` and `FormatSupportsEncryption`
- `CreateArchive(format, output, files, password, ZipEncryption::ZipCrypto, progress, error)`, and a matching last parameter on `CreateEncryptedZip`, for legacy encryption. `ZipEncryption::Aes256` is the default everywhere.

**Guarantees, for every format:**
- **Checked first:** all inputs are checked before anything is written: missing, unreadable or locked files, folders, duplicate names, an output that is also an input, and a missing output folder.
- **No partial archives:** a failed or cancelled run leaves no partial archive, and an existing file at the output path stays unchanged. ZIP builds in libzip's temporary file; TAR writes to a temporary file in the same folder and renames it at the end.
- **Unicode names:** non-English file names are stored as UTF-8. ZIP sets the UTF-8 flag; TAR uses pax extended headers.

**Extraction is safe against hostile archives.** The whole archive is checked before anything is written, and nothing is written if any check fails:
- **Paths:** entries that would land outside the chosen folder (`..\`, absolute paths, drive letters, the "Zip Slip" attack) are refused (`UnsafeEntry`). So are names Windows can't use safely: device names like `CON` or `NUL`, `:` (alternate data streams), `<>"|?*`, and trailing dots or spaces.
- **Links:** symbolic and hard links, and device entries, are skipped and reported in `summary.skipped`, because a link could point anywhere.
- **Existing files:** they're only replaced with `overwrite`; otherwise the result is `FileExists`, naming the first one. Two entries mapping to the same file (e.g. differing only by case) are refused.
- **Password and space:** a wrong or missing password (`WrongPassword` / `PasswordRequired`) and a full disk are detected up front.
- **Clean failure:** each file is written to a temporary name and renamed when complete. A failure or cancel removes every file and folder the run created; files that were already there are left alone.

### Changes from the runbook

- **Failures don't leave a broken archive.** On error the runbook called `zip_close`, which writes a partial ZIP. The engine uses `zip_discard`.
- **Same-named files are rejected.** Entries are stored by file name, and with `ZIP_FL_OVERWRITE` two `report.txt` files from different folders silently overwrote each other. They now fail with `DuplicateEntryName`, as do names that differ only by case.
- **UTF-8 file names.** The runbook's `CT2A` converts with the ANSI code page, which mangles non-English file names; paths and entry names are now UTF-8.
- **ASCII-only ZIP passwords.** Passwords may use English letters, digits, spaces and punctuation. ZIP has no standard password encoding. Testing showed that 7-Zip decodes ZIP passwords with the computer's ANSI code page and refuses to *create* ZIPs with non-ASCII passwords. So `pässwörd` might open on one PC but not on one with different regional settings.
- **Progress and cancel.** The work runs on a worker thread (in the app) with progress and cancel.
- **Clear errors.** `GetLastErrorMessage()` names the file involved. New `Status` values: `InvalidArgument`, `DuplicateEntryName`, `FileReadError` and `Cancelled`.
- **UI.** Instead of hard-coded paths there's a file picker, drag-and-drop, a Save As box, password confirmation and a no-password warning.

### Opening the archives

- **Encrypted ZIP (AES-256, the default):** use this tool's **Extract archive** tab or `zipcli extract`, or 7-Zip, WinRAR or WinZip. Windows Explorer can't open AES-encrypted ZIPs.
- **Encrypted ZIP (legacy ZipCrypto):** Windows Explorer opens it too, and asks for the password. This was tested with Explorer's own ZIP handler on Windows 11.
- **TAR, TAR.GZ, TAR.BZ2, TAR.XZ:** use 7-Zip, WinRAR, or `tar -xf` (built into Windows 10 and later, macOS and Linux).
- **Windows' own `tar.exe` and file names:** it can only handle names representable in the system's ANSI code page. On a Western-European Windows, for example, it skips a file named in Chinese, and crashes if asked to create such an archive itself. Archives from this tool store such names correctly, and 7-Zip and GNU tar extract them.
- **Windows' own `tar.exe` and `.tar.xz`:** current Windows 11 builds include xz support, but Windows 10 and Windows Server 2019/2022 ship an older `tar.exe` without it. On those systems, use 7-Zip to open `.tar.xz` files. `tar --version` shows the support: `liblzma/` in the output means `.tar.xz` works. If you're producing archives for such machines, `.tar.gz` is the safe choice. The tests check this the same way, and skip `tar.exe` checks for formats the installed version can't decompress.

## Build

Requires Visual Studio 2022 with *Desktop development with C++*, *C++ MFC for latest v143 build tools* and the *vcpkg package manager* component.

1. Open `ZipAppSolution.sln`.
2. Choose **Release | x64**.
3. Build with **Build → Build Solution** (`Ctrl+Shift+B`).

Or from a command prompt:

```bash
"C:/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe" ZipAppSolution.sln -m -p:Configuration=Release -p:Platform=x64
```

There's no separate `vcpkg install` step. Dependencies are declared in `vcpkg.json` and installed on the first build, into `vcpkg_installed\`:
- libzip, with AES via Windows' built-in CNG crypto library
- libarchive, with gzip, bzip2 and xz support

That first build takes a few minutes. OpenSSL isn't needed.

- **No DLLs:** everything, including MFC and the C runtime, is linked statically. `ZipAppMFC.exe` and `zipcli.exe` in `bin\x64\Release\` are single files that need no redistributable.
- **C++ standard:** the code uses only C++11 features. MSVC's oldest selectable standard is C++14, so the projects use `/std:c++14`.

## Tests

```bash
bin/x64/Release/ZipEngineTests.exe
```

```bash
powershell -ExecutionPolicy Bypass -File ZipCli/CliTest.ps1 -Configuration Release -Platform x64
```

- **`ZipEngineTests`** checks:
  - **ZIP:** AES-256 entries, contents, wrong or missing passwords, and that no plaintext leaks into the archive. The legacy option really writes ZipCrypto (and the default stays AES), reads back, and opens in 7-Zip.
  - **TAR:** every TAR variant, read back with libarchive, including the real compression filter, Unicode names and contents.
  - **Shared rules:** validation, progress, and that cancelling keeps the previous archive.
  - **Helpers:** format detection and listing.
  - **Extraction:** round trips for every format, right/wrong/missing passwords, hostile entry names in both ZIP and TAR, links, folders stored by other tools, existing files, cancel and rollback, and AES-256 and ZipCrypto ZIPs made by 7-Zip.
  - **External tools:** archives are cross-checked with **7-Zip** (`7z t`) and **Windows `tar.exe`** (extract and compare).
- **`CliTest.ps1`** runs `zipcli.exe` end to end. It covers every option and exit code, wildcards, `--force`, `--password-env` and each format, and opens the results with 7-Zip and `tar.exe`. For `extract` it covers the default folder, `--force`, wrong and missing passwords, every TAR variant, a Zip Slip archive and ZIPs made by 7-Zip. For `--legacy` / `--encryption` it checks the method in the ZIP header and the round trip.

CI (`.github/workflows/build.yml`) builds Debug and Release for x64 and x86 and runs both test suites. 7-Zip and `tar.exe` are preinstalled on GitHub's Windows runners.

**Releases.** Every push to `main` whose builds and tests all pass is published as a GitHub release `v1.2.<run number>`, with release notes generated from the commits since the previous release. It has two downloads:
- `ZipApp-x64.zip`
- `ZipApp-x86.zip`

Each contains `ZipAppMFC.exe`, `zipcli.exe`, their PDBs (for crash analysis) and this README. To start a new minor or major version, change `VERSION` in the `release` job, and update the version resource in `ZipAppMFC.rc` and `kVersion` in `ZipCli/main.cpp` to match. Pull requests run the builds and tests but never release.
