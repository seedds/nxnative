# Runs nxaot.exe, the recompiler cross-built for Windows (dm\tools\aot\README.md, "Building nxaot for Windows"), on this PC and records the run in the repository: one log
# per step and summary.txt in logs\win64\nxaot\ (logs\README.md; a folder of its own, since dm\host\tests\run_win64_tests.ps1 replaces the *.log files of logs\win64\). From
# the repository root:
#   powershell -NoProfile -ExecutionPolicy Bypass -File dm\tools\aot\tests\run_win64_nxaot.ps1 [-Nxaot build\aot-win64\nxaot.exe] [-Reference <file>] [-OutDir <folder>]
#              [-LogDir logs\win64\nxaot] [-TimeoutSeconds 600]
# The CI artifact nxaot-win64 (.github\workflows\port.yml) holds nxaot.exe and its reference file nxaot_win64_ref.tsv: unzip it into build\aot-win64 of a checkout of the
# same commit. Steps:
#   1. nxaot.exe version: the lock file's rows (llvm 18.1.3, the dynarmic commit), which also shows that the program starts on this Windows (it imports only Windows' DLLs)
#   2. for each row of the reference file (-Reference, default nxaot_win64_ref.tsv beside nxaot.exe; columns: name, the input from the repository root, the SHA-256 of the
#      DLL the Linux nxaot made from it, the options it was made with), the same input packed by nxaot.exe with the same options into <OutDir>\<name>.dll (default:
#      check\ beside nxaot.exe; a DLL's file name is in its export directory, so it is the same name as the Linux one), and the two SHA-256 compared. The two programs use
#      different C++ standard libraries (libstdc++ on Linux, libc++ here) and LLVM built by different compilers, so whether they make the same bytes is what this run answers:
#      a difference is a finding, reported with both hashes (and, when the Linux DLL <name>.dll lies beside the reference file, both sizes and the first differing offset)
# Exit status: 0 when every step ran and every DLL is the Linux one byte for byte; 3 when every step ran and a DLL differs (the finding: commit logs\win64\nxaot and push);
# 1 when a step failed (nxaot.exe did not start, a pack failed or timed out); 2 when nxaot.exe, the reference file or an input is missing. Windows PowerShell 5.1 (stock on
# Windows 10) or PowerShell 7
param(
    [string]$Nxaot = '',
    [string]$Reference = '',
    [string]$OutDir = '',
    [string]$LogDir = '',
    [int]$TimeoutSeconds = 600
)
Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

$invoked = $MyInvocation.Line
if (-not $invoked) { $invoked = [Environment]::CommandLine }
$startedIn = (Get-Location).Path
$repo = (Resolve-Path -LiteralPath (Join-Path (Join-Path (Join-Path (Join-Path $PSScriptRoot '..') '..') '..') '..')).Path
$aotDir = Join-Path (Join-Path $repo 'build') 'aot-win64'
if (-not $Nxaot) { $Nxaot = Join-Path $aotDir 'nxaot.exe' }
if (-not (Test-Path -LiteralPath $Nxaot -PathType Leaf)) { Write-Host "no nxaot.exe: $Nxaot (the CI artifact nxaot-win64 unzipped into build\aot-win64, or -Nxaot)"; exit 2 }
$Nxaot = (Resolve-Path -LiteralPath $Nxaot).Path
if (-not $Reference) { $Reference = Join-Path (Split-Path -Parent $Nxaot) 'nxaot_win64_ref.tsv' }
if (-not (Test-Path -LiteralPath $Reference -PathType Leaf)) { Write-Host "no reference file: $Reference (the artifact nxaot-win64 has it beside nxaot.exe, or -Reference)"; exit 2 }
$Reference = (Resolve-Path -LiteralPath $Reference).Path
if (-not $OutDir) { $OutDir = Join-Path (Split-Path -Parent $Nxaot) 'check' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path
if (-not $LogDir) { $LogDir = Join-Path (Join-Path (Join-Path $repo 'logs') 'win64') 'nxaot' }
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$LogDir = (Resolve-Path -LiteralPath $LogDir).Path
Get-ChildItem -LiteralPath $LogDir -File | Where-Object { $_.Name -like '*.log' -or $_.Name -eq 'summary.txt' } | Remove-Item -Force

$onWindows = $env:OS -eq 'Windows_NT'
if ($onWindows) {
    # A crash ends at once with its exception code instead of waiting on a Windows Error Reporting dialog: child processes inherit the error mode
    try {
        Add-Type -Namespace NxNative -Name ErrorMode -MemberDefinition '[DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint mode);'
        [void][NxNative.ErrorMode]::SetErrorMode(0x0003)   # SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX
    } catch { Write-Host "note: the error mode could not be set ($_): a crash may wait on an error dialog until its time limit" }
}
$utf8 = New-Object System.Text.UTF8Encoding $false   # logs are UTF-8 without a byte order mark
function Now { (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ') }
function Get-RepoGit([string[]]$GitArgs) {   # git's output, or $null when git is missing or fails (stderr must not become an error under 'Stop' in Windows PowerShell 5.1)
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { $out = & git -C $repo @GitArgs 2>$null; if ($LASTEXITCODE -ne 0) { return $null }; return $out }
    catch { return $null }
    finally { $ErrorActionPreference = $old }
}
function Get-Sha256([string]$path) {   # Get-FileHash is missing from some PowerShell 5.1 installs (no Microsoft.PowerShell.Utility module update): .NET's own
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $s = [System.IO.File]::OpenRead($path)
    try { return (($sha.ComputeHash($s) | ForEach-Object { $_.ToString('x2') }) -join '') } finally { $s.Dispose(); $sha.Dispose() }
}
$commit = Get-RepoGit @('rev-parse', 'HEAD')
if (-not $commit) { $commit = 'unknown' }
$dirty = Get-RepoGit @('status', '--porcelain', '--untracked-files=no', '--', '.', ':(exclude)logs')
if ($dirty) { $commit = "$commit (with uncommitted changes)" }
$started = Now

# One run of nxaot.exe with its output in <LogDir>\<name>.log (standard output and standard error together, in order, through the shell); returns @(exit code, seconds, timed out)
function Invoke-Nxaot([string]$name, [string[]]$nxArgs) {
    $log = Join-Path $LogDir "$name.log"
    $argText = ''
    foreach ($a in $nxArgs) { if ($a -match '[\s"]') { $argText += " `"$a`"" } else { $argText += " $a" } }
    if ($onWindows) { $inner = "`"$Nxaot`"$argText >> `"$log`" 2>&1" } else { $inner = "'$Nxaot'$argText >> '$log' 2>&1" }
    [System.IO.File]::WriteAllText($log, "date: $(Now)`ncommit: $commit`ncommand: `"$Nxaot`"$argText`n`n", $utf8)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = $OutDir
    if ($onWindows) { $psi.FileName = $env:ComSpec; $psi.Arguments = "/d /s /c `"$inner`"" }
    else { $psi.FileName = '/bin/sh'; $psi.ArgumentList.Add('-c'); $psi.ArgumentList.Add($inner) }
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $p = [System.Diagnostics.Process]::Start($psi)
    $timedOut = -not $p.WaitForExit($TimeoutSeconds * 1000)
    if ($timedOut) {
        if ($onWindows) { Start-Process -FilePath "$env:SystemRoot\System32\taskkill.exe" -ArgumentList '/T', '/F', '/PID', $p.Id -NoNewWindow -Wait }
        else { $p.Kill($true) }
        $p.WaitForExit()
    }
    $secs = [math]::Round($watch.Elapsed.TotalSeconds, 1)
    $rc = $p.ExitCode
    $hex = '0x{0:X8}' -f $rc   # Windows reports crashes as NTSTATUS codes: 0xC0000005 access violation, 0xC00000FD stack overflow, 0xC0000409 fast fail (abort, a failed assert)
    [System.IO.File]::AppendAllText($log, "`nexit: $rc ($hex, $secs s)$(if ($timedOut) { ', killed after the time limit' })`n", $utf8)
    return @($rc, $secs, $timedOut)
}

$results = New-Object System.Collections.Generic.List[string]
$failed = 0; $same = 0; $different = 0; $usage = 0

# 1. nxaot.exe version
$r = Invoke-Nxaot 'version' @('version')
$versionText = (Get-Content -LiteralPath (Join-Path $LogDir 'version.log') -Encoding UTF8 | Where-Object { $_ -match '^[a-z]+\t' }) -join '; '
if ($r[2]) { $line = "TIMEOUT version (killed after $TimeoutSeconds s; see version.log)"; $failed++ }
elseif ($r[0] -ne 0 -or -not $versionText) { $line = "FAIL version (exit $($r[0]) = $('0x{0:X8}' -f $r[0]); see version.log)"; $failed++ }
else { $line = "PASS version ($($r[1]) s): $($versionText -replace "`t", ' ')" }
Write-Host $line; $results.Add($line)

# 2. The reference file's packs
$refHeader = New-Object System.Collections.Generic.List[string]
foreach ($l in @(Get-Content -LiteralPath $Reference -Encoding UTF8)) {
    if (-not $l) { continue }
    if ($l.StartsWith('#')) { $refHeader.Add($l); continue }
    $f = $l.Split("`t")
    if ($f.Count -lt 4) { $line = "FAIL reference row (not name, input, SHA-256, options): $l"; $usage++; Write-Host $line; $results.Add($line); continue }
    $name = $f[0]; $inputRel = $f[1]; $linuxSha = $f[2].ToLowerInvariant(); $opts = @($f[3] -split ' ' | Where-Object { $_ -ne '' })
    $inputPath = Join-Path $repo ($inputRel.Replace('/', [IO.Path]::DirectorySeparatorChar))
    if (-not (Test-Path -LiteralPath $inputPath -PathType Leaf)) { $line = "FAIL $name (no input $inputPath: a checkout of the commit the reference file was made at)"; $usage++; Write-Host $line; $results.Add($line); continue }
    $dll = Join-Path $OutDir "$name.dll"
    if (Test-Path -LiteralPath $dll) { Remove-Item -LiteralPath $dll -Force }
    $r = Invoke-Nxaot $name (@('build', $inputPath) + $opts + @('--shared', $dll))
    if ($r[2]) { $line = "TIMEOUT $name (killed after $TimeoutSeconds s; see $name.log)"; $failed++ }
    elseif ($r[0] -ne 0 -or -not (Test-Path -LiteralPath $dll -PathType Leaf)) { $line = "FAIL $name (exit $($r[0]) = $('0x{0:X8}' -f $r[0]), $($r[1]) s; see $name.log)"; $failed++ }
    else {
        $winSha = Get-Sha256 $dll
        $winSize = (Get-Item -LiteralPath $dll).Length
        if ($winSha -eq $linuxSha) { $line = "SAME $name ($($r[1]) s): $winSha, $winSize bytes, as the Linux nxaot made it"; $same++ }
        else {
            $line = "DIFFERENT $name ($($r[1]) s): Windows $winSha ($winSize bytes), Linux $linuxSha"
            $linuxDll = Join-Path (Split-Path -Parent $Reference) "$name.dll"
            if (Test-Path -LiteralPath $linuxDll -PathType Leaf) {
                $a = [System.IO.File]::ReadAllBytes($dll); $b = [System.IO.File]::ReadAllBytes($linuxDll)
                $n = [math]::Min($a.Length, $b.Length); $first = -1; $count = 0
                for ($i = 0; $i -lt $n; $i++) { if ($a[$i] -ne $b[$i]) { if ($first -lt 0) { $first = $i }; $count++ } }
                $line += " ($($b.Length) bytes, $(Get-Sha256 $linuxDll) as read); first difference at offset 0x$('{0:X}' -f $first), $count of the first $n bytes differ"
            }
            $different++
        }
    }
    Write-Host $line; $results.Add($line)
}

$os = [Environment]::OSVersion.VersionString
try { $os = (Get-CimInstance Win32_OperatingSystem -ErrorAction Stop | ForEach-Object { "$($_.Caption) $($_.Version) build $($_.BuildNumber)" }) } catch { }
$cpu = $env:PROCESSOR_IDENTIFIER
try { $cpu = (Get-CimInstance Win32_Processor -ErrorAction Stop | Select-Object -First 1).Name } catch { }
$summary = New-Object System.Collections.Generic.List[string]
$summary.Add("date: $started"); $summary.Add("commit: $commit"); $summary.Add("command: $invoked"); $summary.Add('')
$summary.Add("ran in: $startedIn"); $summary.Add("system: $os"); $summary.Add("cpu: $cpu")
$summary.Add("nxaot: $Nxaot ($((Get-Item -LiteralPath $Nxaot).Length) bytes, SHA-256 $(Get-Sha256 $Nxaot))")
$summary.Add("reference: $Reference"); foreach ($h in $refHeader) { $summary.Add("  $h") }
$summary.Add("packs written to: $OutDir"); $summary.Add('')
foreach ($l in $results) { $summary.Add($l) }
$summary.Add('')
$summary.Add("packs: $same the same as Linux's, $different different, $failed failed; $usage reference rows or inputs missing")
[System.IO.File]::WriteAllText((Join-Path $LogDir 'summary.txt'), (($summary -join "`n") + "`n"), $utf8)
Write-Host "summary: $(Join-Path $LogDir 'summary.txt')"
if ($failed) { exit 1 }
if ($usage) { exit 2 }
if ($different) { exit 3 }
exit 0
