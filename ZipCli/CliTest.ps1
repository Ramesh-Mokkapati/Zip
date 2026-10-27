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
        $dest = "x_$($ext.Replace('.', '_'))"; New-Item -ItemType Directory $dest | Out-Null
        & $tarExe -xf "logs.$ext" -C $dest
        Check ($LASTEXITCODE -eq 0 -and (Get-Content "$dest\b.log" -Raw) -eq 'bravo') "Windows tar.exe extracts logs.$ext"
    }
    $r = Invoke-Cli @('create', '--format', 'tar.xz', 'named.zip', 'c.txt')
    Check ($r.Code -eq 0 -and $r.Out -match 'TAR\.XZ') '--format overrides the extension'
    & $tarExe -tf named.zip | Out-Null;                          Check ($LASTEXITCODE -eq 0) '...and the file really is a tar.xz'
}
finally {
    Pop-Location
    Remove-Item Env:ZIPCLI_TEST_PW -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "$script:checks checks, $script:failures failed"
if ($script:failures -ne 0) { exit 1 }
