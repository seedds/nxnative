# Runs the host unit tests of a Windows build folder on this PC and records the run in the repository: one log per test and summary.txt in logs\win64\
# (logs\README.md). The tree is cross-built in Docker with llvm-mingw (docker\README.md); this script only runs what was built. From the repository root:
#   powershell -NoProfile -ExecutionPolicy Bypass -File dm\host\tests\run_win64_tests.ps1 [-BuildDir build\win64] [-LogDir logs\win64] [-TimeoutSeconds 120]
# The runs come from <BuildDir>\host_tests.tsv (written by dm\host\CMakeLists.txt); the old *.log files and summary.txt in <LogDir> are removed first.
# Exit status: 0 when every run passed, 1 when one did not, 2 when the build folder or its test list is missing. Windows PowerShell 5.1 (stock on Windows 10) or PowerShell 7.
# dm/host/tests/run_host_tests.sh does the same from Git Bash (and on Linux)
param(
    [string]$BuildDir = '',
    [string]$LogDir = '',
    [int]$TimeoutSeconds = 120
)
Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

$invoked = $MyInvocation.Line
if (-not $invoked) { $invoked = [Environment]::CommandLine }
$startedIn = (Get-Location).Path
$repo = (Resolve-Path -LiteralPath (Join-Path (Join-Path (Join-Path $PSScriptRoot '..') '..') '..')).Path
if (-not $BuildDir) { $BuildDir = Join-Path (Join-Path $repo 'build') 'win64' }
if (-not (Test-Path -LiteralPath $BuildDir -PathType Container)) { Write-Host "no build folder: $BuildDir (cross-build it first: docker\README.md)"; exit 2 }
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$manifest = Join-Path $BuildDir 'host_tests.tsv'
if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) { Write-Host "$manifest is missing: configure and build the tree first (docker\README.md)"; exit 2 }
$lines = @(Get-Content -LiteralPath $manifest -Encoding UTF8)
$platform = 'unknown'
foreach ($l in $lines) { if ($l -like '# platform: *') { $platform = $l.Substring(12).Trim(); break } }
if (-not $LogDir) { $LogDir = Join-Path (Join-Path $repo 'logs') $platform }
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$LogDir = (Resolve-Path -LiteralPath $LogDir).Path
Get-ChildItem -LiteralPath $LogDir -File | Where-Object { $_.Name -like '*.log' -or $_.Name -eq 'summary.txt' } | Remove-Item -Force

$onWindows = $env:OS -eq 'Windows_NT'
if ($onWindows) {
    # A crashing test ends at once with its exception code instead of waiting on a Windows Error Reporting dialog: child processes inherit the error mode
    try {
        Add-Type -Namespace NxNative -Name ErrorMode -MemberDefinition '[DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint mode);'
        [void][NxNative.ErrorMode]::SetErrorMode(0x0003)   # SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX
    } catch { Write-Host "note: the error mode could not be set ($_): a crashing test may wait on an error dialog until its time limit" }
}
$utf8 = New-Object System.Text.UTF8Encoding $false   # logs are UTF-8 without a byte order mark, as the tests write them
function Now { (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ') }
function Get-RepoGit([string[]]$GitArgs) {   # git's output, or $null when git is missing or fails (stderr must not become an error: Windows PowerShell 5.1 turns it into one under 'Stop')
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { $out = & git -C $repo @GitArgs 2>$null; if ($LASTEXITCODE -ne 0) { return $null }; return $out }
    catch { return $null }
    finally { $ErrorActionPreference = $old }
}
$commit = Get-RepoGit @('rev-parse', 'HEAD')
if (-not $commit) { $commit = 'unknown' }
$dirty = Get-RepoGit @('status', '--porcelain', '--untracked-files=no', '--', '.', ':(exclude)logs')
if ($dirty) { $commit = "$commit (with uncommitted changes)" }
$started = Now
$tmpDir = Join-Path $BuildDir 'tmp'

$results = New-Object System.Collections.Generic.List[string]
$total = 0; $passed = 0; $failed = 0
foreach ($l in $lines) {
    if (-not $l -or $l.StartsWith('#')) { continue }
    $f = $l.Split("`t")
    if ($f.Count -lt 4) { continue }
    $name = $f[0]; $exe = $f[1]; $targs = $f[2]; $needTmp = $f[3] -eq 'tmpdir'
    $total++
    $log = Join-Path $LogDir "$name.log"
    $exePath = Join-Path $BuildDir $exe
    $argText = ''; if ($targs -ne '-') { $argText = ' ' + $targs }
    # The test runs through the shell with stdout and stderr redirected to the log together, so the log keeps the order in which the test wrote them
    if ($onWindows) {
        $envText = ''; if ($needTmp) { $envText = "set TMPDIR=$tmpDir&& " }
        $cmdline = "cd /d `"$BuildDir`" && $envText`"$exePath`"$argText"
        $inner = "`"$exePath`"$argText >> `"$log`" 2>&1"
    } else {   # PowerShell 7 elsewhere: lets the runner itself be checked against a Linux tree
        $envText = ''; if ($needTmp) { $envText = "TMPDIR=$tmpDir " }
        $cmdline = "cd $BuildDir && $envText$exePath$argText"
        $inner = "'$exePath'$argText >> '$log' 2>&1"
    }
    [System.IO.File]::WriteAllText($log, "date: $(Now)`ncommit: $commit`ncommand: $cmdline`n`n", $utf8)
    if ($needTmp) { New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = $BuildDir
    if ($needTmp) { $psi.EnvironmentVariables['TMPDIR'] = $tmpDir }
    if ($onWindows) { $psi.FileName = $env:ComSpec; $psi.Arguments = "/d /s /c `"$inner`"" }
    else { $psi.FileName = '/bin/sh'; $psi.ArgumentList.Add('-c'); $psi.ArgumentList.Add($inner) }
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $p = [System.Diagnostics.Process]::Start($psi)
    $timedOut = -not $p.WaitForExit($TimeoutSeconds * 1000)
    if ($timedOut) {
        if ($onWindows) { Start-Process -FilePath "$env:SystemRoot\System32\taskkill.exe" -ArgumentList '/T', '/F', '/PID', $p.Id -NoNewWindow -Wait }   # the shell and the test under it
        else { $p.Kill($true) }
        $p.WaitForExit()
    }
    $secs = [math]::Round($watch.Elapsed.TotalSeconds, 1)
    $rc = $p.ExitCode
    $hex = '0x{0:X8}' -f $rc   # Windows reports crashes as NTSTATUS codes: 0xC0000005 access violation, 0xC0000409 fast fail (also abort() and a failed assert on the UCRT)
    [System.IO.File]::AppendAllText($log, "`nexit: $rc ($hex, $secs s)$(if ($timedOut) { ', killed after the time limit' })`n", $utf8)
    if ($timedOut) { $line = "TIMEOUT $name (killed after $TimeoutSeconds s; see $name.log)"; $failed++ }
    elseif ($rc -eq 0) { $line = "PASS $name ($secs s)"; $passed++ }
    else { $line = "FAIL $name (exit $rc = $hex, $secs s; see $name.log)"; $failed++ }
    Write-Host $line
    $results.Add($line)
}

$totalLine = "$total runs: $passed passed, $failed failed"
$os = [Environment]::OSVersion.VersionString
if ($onWindows) { try { $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'; $os = "$($cv.ProductName) $($cv.DisplayVersion) build $($cv.CurrentBuild).$($cv.UBR)" } catch { } }
$sb = New-Object System.Text.StringBuilder
[void]$sb.Append("date: $started`ncommit: $commit`ncommand: $invoked`ndirectory: $startedIn`nhost: $os`nbuild folder: $BuildDir`n")
[void]$sb.Append("PowerShell: $($PSVersionTable.PSVersion)`n")
foreach ($l in ($lines | Select-Object -Skip 1)) { if ($l.StartsWith('# ')) { [void]$sb.Append($l.Substring(2) + "`n") } }
[void]$sb.Append("`n")
foreach ($r in $results) { [void]$sb.Append("$r`n") }
[void]$sb.Append("`n$totalLine`n")
[System.IO.File]::WriteAllText((Join-Path $LogDir 'summary.txt'), $sb.ToString(), $utf8)
Write-Host "$totalLine (logs in $LogDir)"
if ($total -gt 0 -and $failed -eq 0) { exit 0 } else { exit 1 }
