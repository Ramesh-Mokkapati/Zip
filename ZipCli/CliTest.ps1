<#
.SYNOPSIS
    End-to-end checks of zipcli.exe: commands, options, exit codes, and that the archives it
    writes open in 7-Zip and Windows' tar.exe.
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File ZipCli\CliTest.ps1 -Configuration Release -Platform x64
#>
param(
    [ValidateSet('Debug', 'Release')] [string] $Configuration = 'Release',
    [ValidateSet('x64', 'x86')]       [string] $Platform = 'x64'
)

$ErrorActionPreference = 'Stop'
$platformDir = if ($Platform -eq 'x86') { 'Win32' } else { 'x64' }
$cli = (Resolve-Path (Join-Path $PSScriptRoot "..\bin\$platformDir\$Configuration\zipcli.exe")).Path
$sevenZip = @('C:\Program Files\7-Zip\7z.exe', 'C:\Program Files (x86)\7-Zip\7z.exe') | Where-Object { Test-Path $_ } | Select-Object -First 1
$tarExe = Join-Path $env:WINDIR 'System32\tar.exe'
# `tar --version` lists the decompressors this tar.exe was built with. Windows 10 and Windows
# Server 2019/2022 ship an older bsdtar without liblzma; given a .tar.xz it tries to run an
# external "xz" program and can hang, so formats it cannot decompress are skipped.
$tarVersion = if (Test-Path $tarExe) { (& $tarExe --version | Select-Object -First 1) } else { '' }
Write-Host "Windows tar.exe: $tarVersion"
function Test-TarSupports([string] $ext) {
    switch -Regex ($ext) {
        '^(tar\.gz|tgz)$'   { return $tarVersion -match 'zlib/' }
        '^(tar\.bz2|tbz2)$' { return $tarVersion -match 'bz2lib/' }
        '^(tar\.xz|txz)$'   { return $tarVersion -match 'liblzma/' }
        default             { return $tarVersion -ne '' }
    }
}

# Runs tar.exe with a 60 s limit (a hung tar.exe is killed); returns its exit code, or -2 on timeout.
function Invoke-Tar([string[]] $TarArgs) {
    $p = Start-Process -FilePath $tarExe -ArgumentList $TarArgs -NoNewWindow -PassThru
    # Windows PowerShell 5.1 only keeps ExitCode after WaitForExit(timeout) if the process
    # handle was opened first; reading .Handle does that.
    $null = $p.Handle
    if (-not $p.WaitForExit(60000)) { $p.Kill(); return -2 }
    return $p.ExitCode
}

$work = Join-Path ([IO.Path]::GetTempPath()) ("ZipCliTest_" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $work | Out-Null
$script:failures = 0
$script:checks = 0

function Check([bool] $condition, [string] $what) {
    $script:checks++
    if ($condition) { Write-Host "  ok   $what" } else { $script:failures++; Write-Host "  FAIL $what" -ForegroundColor Red }
}

# Runs zipcli with the given arguments; returns @{ Code; Out; Err }.
function Invoke-Cli([string[]] $CliArgs) {
    $outFile = Join-Path $work 'stdout.txt'; $errFile = Join-Path $work 'stderr.txt'
    # stdin from an empty file, not the console: zipcli must never wait for a password here.
    $inFile = Join-Path $work 'stdin.txt'; if (-not (Test-Path $inFile)) { New-Item -ItemType File $inFile | Out-Null }
    $p = Start-Process -FilePath $cli -ArgumentList $CliArgs -NoNewWindow -Wait -PassThru `
                       -RedirectStandardInput $inFile -RedirectStandardOutput $outFile -RedirectStandardError $errFile
    return @{ Code = $p.ExitCode
              Out  = [IO.File]::ReadAllText($outFile, [Text.Encoding]::UTF8)
              Err  = [IO.File]::ReadAllText($errFile, [Text.Encoding]::UTF8) }
}

try {
    Push-Location $work
    'alpha'   | Set-Content -NoNewline -Encoding ASCII a.log
    'bravo'   | Set-Content -NoNewline -Encoding ASCII b.log
    'charlie' | Set-Content -NoNewline -Encoding ASCII c.txt

    Write-Host 'Usage and errors'
    $r = Invoke-Cli @('--help');                         Check ($r.Code -eq 0 -and $r.Out -match 'zipcli create') '--help prints usage, exit 0'
    $r = Invoke-Cli @('--version');                      Check ($r.Code -eq 0 -and $r.Out -match '^zipcli \d') '--version'
    $r = Invoke-Cli @('frobnicate');                     Check ($r.Code -eq 1) 'unknown command -> exit 1'
    $r = Invoke-Cli @('create', 'x.zip');                Check ($r.Code -eq 1 -and $r.Err -match 'no files') 'no files -> exit 1'
    $r = Invoke-Cli @('create', 'x.rar', 'c.txt');       Check ($r.Code -eq 1 -and $r.Err -match 'cannot tell the format') 'unknown extension -> exit 1'
    $r = Invoke-Cli @('create', '--bogus', 'x.zip', 'c.txt'); Check ($r.Code -eq 1) 'unknown option -> exit 1'
    $r = Invoke-Cli @('create', 'x.zip', 'missing.txt'); Check ($r.Code -eq 2 -and $r.Err -match 'missing.txt' -and -not (Test-Path x.zip)) 'missing file -> exit 2, no archive'
    $r = Invoke-Cli @('create', 'x.zip', 'nomatch*.dat'); Check ($r.Code -eq 2 -and $r.Err -match 'no files match') 'wildcard with no match -> exit 2'
    $r = Invoke-Cli @('create', '--password', 'pw', 'x.tar.gz', 'c.txt'); Check ($r.Code -eq 1 -and $r.Err -match 'cannot be encrypted') 'password with tar -> exit 1'

    Write-Host 'Encrypted ZIP with --password-env and wildcards'
    $env:ZIPCLI_TEST_PW = 'Env-Pass-123'
    $r = Invoke-Cli @('create', '--password-env', 'ZIPCLI_TEST_PW', 'secret.zip', '*.log', 'c.txt')
    Check ($r.Code -eq 0 -and $r.Out -match 'AES-256 encrypted' -and $r.Out -match '3 file') 'create secret.zip (3 files, encrypted)'
    $r = Invoke-Cli @('list', 'secret.zip')
    Check ($r.Code -eq 0 -and $r.Out -match 'a\.log' -and $r.Out -match 'b\.log' -and $r.Out -match 'c\.txt' -and $r.Out -match 'encrypted') 'list shows 3 encrypted entries'
    if ($sevenZip) {
        & $sevenZip t -pEnv-Pass-123 secret.zip -bso0 -bsp0;      Check ($LASTEXITCODE -eq 0) '7-Zip opens it with the password'
        & $sevenZip t -pwrong secret.zip -bso0 -bse0 -bsp0;       Check ($LASTEXITCODE -ne 0) '7-Zip rejects a wrong password'
    }

    Write-Host 'Existing archive and --force'
    $before = (Get-Item secret.zip).Length
    $r = Invoke-Cli @('create', 'secret.zip', 'c.txt');          Check ($r.Code -eq 2 -and $r.Err -match '--force' -and (Get-Item secret.zip).Length -eq $before) 'existing archive kept without --force'
    $r = Invoke-Cli @('create', '--force', '-q', 'secret.zip', 'c.txt'); Check ($r.Code -eq 0 -and $r.Out -eq '') '--force replaces it; -q prints nothing'

    Write-Host 'TAR formats'
    foreach ($ext in 'tar', 'tar.gz', 'tgz', 'tar.bz2', 'tar.xz') {
        $r = Invoke-Cli @('create', "logs.$ext", '*.log')
        Check ($r.Code -eq 0) "create logs.$ext"
        $r = Invoke-Cli @('list', "logs.$ext")
        Check ($r.Code -eq 0 -and $r.Out -match 'a\.log' -and $r.Out -match '2 entries') "list logs.$ext"
        if (Test-TarSupports $ext) {
            $dest = "x_$($ext.Replace('.', '_'))"; New-Item -ItemType Directory $dest | Out-Null
            $code = Invoke-Tar @('-xf', "logs.$ext", '-C', $dest)
            Check ($code -eq 0 -and (Get-Content "$dest\b.log" -Raw) -eq 'bravo') "Windows tar.exe extracts logs.$ext (exit $code)"
        } else {
            Write-Host "  skip Windows tar.exe cannot decompress .$ext on this Windows version"
        }
        if ($sevenZip -and $ext -ne 'tar') {
            & $sevenZip t "logs.$ext" -bso0 -bsp0;               Check ($LASTEXITCODE -eq 0) "7-Zip verifies logs.$ext"
        }
    }
    $r = Invoke-Cli @('create', '--format', 'tar.xz', 'named.zip', 'c.txt')
    Check ($r.Code -eq 0 -and $r.Out -match 'TAR\.XZ') '--format overrides the extension'
    # xz streams start with the magic bytes FD 37 7A 58 5A 00 ("\xFD7zXZ\0").
    $magic = ([IO.File]::ReadAllBytes((Join-Path $work 'named.zip')) | Select-Object -First 6 | ForEach-Object { $_.ToString('X2') }) -join ' '
    Check ($magic -eq 'FD 37 7A 58 5A 00') "...and the file really is xz-compressed ($magic)"

    Write-Host 'Extract'
    $r = Invoke-Cli @('--help');                                 Check ($r.Out -match 'zipcli extract') '--help describes extract'
    $r = Invoke-Cli @('extract');                                Check ($r.Code -eq 1 -and $r.Err -match 'no archive') 'extract without an archive -> exit 1'
    $r = Invoke-Cli @('extract', 'missing.zip');                 Check ($r.Code -eq 2 -and $r.Err -match 'not found') 'extract a missing archive -> exit 2'
    $r = Invoke-Cli @('extract', 'c.txt', '-d', 'notarc');       Check ($r.Code -eq 2 -and -not (Test-Path notarc)) 'extract a file that is not an archive -> exit 2'

    $r = Invoke-Cli @('create', '-q', '--password-env', 'ZIPCLI_TEST_PW', 'enc.zip', '*.log', 'c.txt')
    $r = Invoke-Cli @('extract', '--password-env', 'ZIPCLI_TEST_PW', 'enc.zip')
    Check ($r.Code -eq 0 -and $r.Out -match 'Extracted 3 file' -and (Get-Content enc\c.txt -Raw) -eq 'charlie' -and
           (Get-Content enc\a.log -Raw) -eq 'alpha') 'extract enc.zip into .\enc (the default folder) with --password-env'
    'changed' | Set-Content -NoNewline -Encoding ASCII enc\c.txt
    $r = Invoke-Cli @('extract', '--password-env', 'ZIPCLI_TEST_PW', 'enc.zip')
    Check ($r.Code -eq 2 -and $r.Err -match '--force' -and (Get-Content enc\c.txt -Raw) -eq 'changed') 'existing files are kept without --force'
    $r = Invoke-Cli @('extract', '--force', '-q', '--password-env', 'ZIPCLI_TEST_PW', 'enc.zip')
    Check ($r.Code -eq 0 -and $r.Out -eq '' -and (Get-Content enc\c.txt -Raw) -eq 'charlie') '--force replaces them; -q prints nothing'
    $r = Invoke-Cli @('extract', '--password', 'wrong', 'enc.zip', '-d', 'bad')
    Check ($r.Code -eq 2 -and $r.Err -match 'password' -and -not (Test-Path bad\a.log)) 'wrong password -> exit 2, nothing written'
    $r = Invoke-Cli @('extract', 'enc.zip', '-d', 'nopw')
    Check ($r.Code -eq 2 -and $r.Err -match 'is encrypted' -and -not (Test-Path nopw\a.log)) 'encrypted, no password and no console -> exit 2'
    $r = Invoke-Cli @('extract', '--password', 'pw', '-p', 'enc.zip'); Check ($r.Code -eq 1) 'two password options -> exit 1'

    foreach ($ext in 'tar', 'tar.gz', 'tgz', 'tar.bz2', 'tar.xz') {
        $dest = "out_$($ext.Replace('.', '_'))"
        $r = Invoke-Cli @('x', "logs.$ext", '-d', $dest)
        Check ($r.Code -eq 0 -and (Get-Content "$dest\b.log" -Raw) -eq 'bravo') "extract logs.$ext"
    }

    # A hostile archive: an entry named ..\evil.txt would land outside the chosen folder.
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::Open((Join-Path $work 'evil.zip'), 'Create')
    $w = New-Object IO.StreamWriter ($zip.CreateEntry('safe.txt').Open()); $w.Write('fine'); $w.Dispose()
    $w = New-Object IO.StreamWriter ($zip.CreateEntry('../evil.txt').Open()); $w.Write('gotcha'); $w.Dispose()
    $zip.Dispose()
    New-Item -ItemType Directory jail | Out-Null
    $r = Invoke-Cli @('extract', 'evil.zip', '-d', 'jail\inner')
    Check ($r.Code -eq 2 -and $r.Err -match 'refused' -and -not (Test-Path jail\evil.txt) -and -not (Test-Path jail\inner\safe.txt)) 'Zip Slip entry refused, nothing written'

    Write-Host 'Legacy ZipCrypto encryption'
    $r = Invoke-Cli @('create', '--legacy', 'x.zip', 'c.txt');   Check ($r.Code -eq 1 -and $r.Err -match 'need a password') '--legacy without a password -> exit 1'
    $r = Invoke-Cli @('create', '-e', 'rot13', '--password', 'pw', 'x.zip', 'c.txt'); Check ($r.Code -eq 1 -and $r.Err -match 'unknown encryption') 'unknown --encryption -> exit 1'
    $r = Invoke-Cli @('create', '--legacy', '--password-env', 'ZIPCLI_TEST_PW', 'legacy.zip', '*.log', 'c.txt')
    Check ($r.Code -eq 0 -and $r.Out -match 'ZipCrypto encrypted' -and $r.Err -match 'weak') 'create legacy.zip: reports ZipCrypto and warns that it is weak'
    # Local file header: general-purpose flags at offset 6 (bit 0 = encrypted), method at 8 (99 = AES;
    # ZipCrypto keeps the real method: 8 = Deflate, or 0 = Stored for a file too small to compress).
    $head = [IO.File]::ReadAllBytes((Join-Path $work 'legacy.zip'))
    Check ((($head[6] -band 1) -eq 1) -and $head[8] -ne 99) "...and its entries are ZipCrypto, not AES (flags $($head[6]), method $($head[8]))"
    $r = Invoke-Cli @('create', '-e', 'aes256', '--password-env', 'ZIPCLI_TEST_PW', 'modern.zip', 'c.txt')
    $head = [IO.File]::ReadAllBytes((Join-Path $work 'modern.zip'))
    Check ($r.Code -eq 0 -and $r.Out -match 'AES-256' -and $head[8] -eq 99) '--encryption aes256 writes AES (method 99)'
    $r = Invoke-Cli @('extract', '--password-env', 'ZIPCLI_TEST_PW', 'legacy.zip', '-d', 'legacy_out')
    Check ($r.Code -eq 0 -and (Get-Content legacy_out\b.log -Raw) -eq 'bravo') 'extract legacy.zip'
    $r = Invoke-Cli @('extract', '--password', 'nope', 'legacy.zip', '-d', 'legacy_bad')
    Check ($r.Code -eq 2 -and -not (Test-Path legacy_bad\a.log)) 'legacy.zip with a wrong password -> exit 2'
    if ($sevenZip) {
        & $sevenZip t -pEnv-Pass-123 legacy.zip -bso0 -bsp0;      Check ($LASTEXITCODE -eq 0) '7-Zip opens legacy.zip with the password'
    }

    if ($sevenZip) {
        & $sevenZip a -tzip -mem=AES256 -pSeven-7 seven.zip a.log c.txt -bso0 -bsp0 | Out-Null
        $r = Invoke-Cli @('extract', '--password', 'Seven-7', 'seven.zip', '-d', 'seven')
        Check ($r.Code -eq 0 -and (Get-Content seven\c.txt -Raw) -eq 'charlie') 'extract an AES-256 ZIP made by 7-Zip'
        & $sevenZip a -tzip -mem=ZipCrypto -pSeven-7 seven-legacy.zip b.log -bso0 -bsp0 | Out-Null
        $r = Invoke-Cli @('extract', '--password', 'Seven-7', 'seven-legacy.zip', '-d', 'seven-legacy')
        Check ($r.Code -eq 0 -and (Get-Content seven-legacy\b.log -Raw) -eq 'bravo') 'extract a ZipCrypto ZIP made by 7-Zip'
    }
}
finally {
    Pop-Location
    Remove-Item Env:ZIPCLI_TEST_PW -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "$script:checks checks, $script:failures failed"
if ($script:failures -ne 0) { exit 1 }
