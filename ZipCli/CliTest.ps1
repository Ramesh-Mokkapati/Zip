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
    $p = Start-Process -FilePath $cli -ArgumentList $CliArgs -NoNewWindow -Wait -PassThru `
                       -RedirectStandardOutput $outFile -RedirectStandardError $errFile
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
}
finally {
    Pop-Location
    Remove-Item Env:ZIPCLI_TEST_PW -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "$script:checks checks, $script:failures failed"
if ($script:failures -ne 0) { exit 1 }
