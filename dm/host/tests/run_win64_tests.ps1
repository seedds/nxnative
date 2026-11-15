# Runs the host unit tests of a Windows build folder on this PC and records the run in the repository: one log per test and summary.txt in logs\win64\
# (logs\README.md). The tree is cross-built in Docker with llvm-mingw (docker\README.md); this script only runs what was built. From the repository root:
#   powershell -NoProfile -ExecutionPolicy Bypass -File dm\host\tests\run_win64_tests.ps1 [-BuildDir build\win64] [-LogDir logs\win64] [-TimeoutSeconds 120] [-Packs build\packs\win64]
#     [-PortTools build\port_tools\win-x64]
# The runs come from <BuildDir>\host_tests.tsv (written by dm\host\CMakeLists.txt), then the script tests of <BuildDir>\host_scripts.tsv that run on Windows (t_second_pack,
# with Python: $env:PYTHON, else py -3; t_vk_spirv is Linux only, and summary.txt lists it as "not run on Windows" with the reason); the old *.log files and summary.txt in
# <LogDir> are removed first.
# The AOT tests (t_aot_host, t_aot_module, t_aot_hostfn, t_host_program, and the script test t_second_pack, which has no nxaot on Windows and takes the packs build_packs.sh
# prebuilt for it in <packs>\second_pack) run the recompiled packs of NXAOT_PACKS: -Packs, else the environment's NXAOT_PACKS, else
# build\packs\win64 when it exists (the Windows packs dm/tools/aot/tests/build_packs.sh writes into <out>\win64: docker\README.md); t_aot_host also gets the corpus of the
# checkout (--corpus dm\tools\aot\corpus: the one compiled in is the container's path). Without packs those tests pass with a note (t_second_pack is a SKIP).
# The port's prebuilt tools (dm/tools/publish_port_tools.sh windows: nx_extract.exe and dmsh_xlate_vk.exe, which a player's bundle carries; docker\README.md, "The port's
# tools") of -PortTools, else build\port_tools\win-x64 when it exists, run as two more script tests with the same Python: port_nx_extract (dm/tools/tests/test_nx_extract.py
# with NX_EXTRACT: the usage, a synthetic NSP extracted, a damaged one reported) and port_dmsh_xlate_vk (dm/gfx/tests/test_dmsh_vk.py with XLATE_VK: its Published class,
# the lock's win-x64 row, the embedded source and three shapes translated; the classes that need SPIRV-Tools or SPIRV-Cross skip without them).
# Exit status: 0 when every run passed, 1 when one did not, 2 when the build folder or its test list (or a given packs or port tools folder) is missing. Windows PowerShell 5.1 (stock on
# Windows 10) or PowerShell 7. dm/host/tests/run_host_tests.sh does the same from Git Bash (and on Linux), with NXAOT_PACKS from the environment
param(
    [string]$BuildDir = '',
    [string]$LogDir = '',
    [int]$TimeoutSeconds = 120,
    [string]$Packs = '',
    [string]$PortTools = ''
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

# The Windows packs (DLLs of nxaot build --target windows) the AOT tests load
if (-not $Packs) { $Packs = $env:NXAOT_PACKS }
if (-not $Packs -and $env:OS -eq 'Windows_NT') {   # the DLLs only on Windows (PowerShell 7 elsewhere runs a Linux tree)
    $defaultPacks = Join-Path (Join-Path (Join-Path $repo 'build') 'packs') 'win64'
    if (Test-Path -LiteralPath $defaultPacks -PathType Container) { $Packs = $defaultPacks }
}
if ($Packs) {
    if (-not (Test-Path -LiteralPath $Packs -PathType Container)) { Write-Host "no packs folder: $Packs (dm/tools/aot/tests/build_packs.sh writes <out>\win64: docker\README.md)"; exit 2 }
    $Packs = (Resolve-Path -LiteralPath $Packs).Path
    Write-Host "packs: $Packs (NXAOT_PACKS of the AOT tests)"
} else {
    Write-Host "note: no Windows packs (-Packs, NXAOT_PACKS or build\packs\win64 from dm/tools/aot/tests/build_packs.sh): the AOT tests pass with a note"
}
$corpus = Join-Path (Join-Path (Join-Path (Join-Path $repo 'dm') 'tools') 'aot') 'corpus'

$onWindows = $env:OS -eq 'Windows_NT'
# The port's prebuilt tools (the two script tests after host_scripts.tsv's)
if (-not $PortTools -and $onWindows) {
    $defaultTools = Join-Path (Join-Path (Join-Path $repo 'build') 'port_tools') 'win-x64'
    if (Test-Path -LiteralPath $defaultTools -PathType Container) { $PortTools = $defaultTools }
}
if ($PortTools) {
    if (-not (Test-Path -LiteralPath $PortTools -PathType Container)) { Write-Host "no port tools folder: $PortTools (dm/tools/publish_port_tools.sh writes it: docker\README.md)"; exit 2 }
    $PortTools = (Resolve-Path -LiteralPath $PortTools).Path
    Write-Host "port tools: $PortTools (nx_extract and dmsh_xlate_vk, run by two script tests)"
}
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
    if ($Packs -and ($name -eq 't_aot_host' -or $name -eq 't_aot_targets')) { $argText += " --corpus `"$corpus`"" }   # the checkout's corpus: the compiled-in path is the container's
    # The test runs through the shell with stdout and stderr redirected to the log together, so the log keeps the order in which the test wrote them
    if ($onWindows) {
        $envText = ''; if ($needTmp) { $envText = "set TMPDIR=$tmpDir&& " }
        if ($Packs) { $envText += "set NXAOT_PACKS=$Packs&& " }
        $cmdline = "cd /d `"$BuildDir`" && $envText`"$exePath`"$argText"
        $inner = "`"$exePath`"$argText >> `"$log`" 2>&1"
    } else {   # PowerShell 7 elsewhere: lets the runner itself be checked against a Linux tree
        $envText = ''; if ($needTmp) { $envText = "TMPDIR=$tmpDir " }
        if ($Packs) { $envText += "NXAOT_PACKS=$Packs " }
        $cmdline = "cd $BuildDir && $envText$exePath$argText"
        $inner = "'$exePath'$argText >> '$log' 2>&1"
    }
    [System.IO.File]::WriteAllText($log, "date: $(Now)`ncommit: $commit`ncommand: $cmdline`n`n", $utf8)
    if ($needTmp) { New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = $BuildDir
    if ($needTmp) { $psi.EnvironmentVariables['TMPDIR'] = $tmpDir }
    if ($Packs) { $psi.EnvironmentVariables['NXAOT_PACKS'] = $Packs }
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

# The script tests (<BuildDir>\host_scripts.tsv, host_script_test() in dm/host/CMakeLists.txt: Python drivers and shell scripts), after the programs, each with its own log in the
# same form. A Windows tree lists only the scripts that run on Windows; the others are "not run on Windows: <name> (<why>)" lines of host_tests.tsv, which the summary copies.
# Columns: name, interpreter (python3: $env:PYTHON, else py -3, python3 or python on PATH; sh: sh on PATH, as Git's), script (from the repository root), arguments and
# environment ({build}, {repo}: this machine's folders; "" an empty argument), the variables it reads from this environment (passed through), the exit code of a skip
$scriptsFile = Join-Path $BuildDir 'host_scripts.tsv'
$sTotal = 0; $sPassed = 0; $sFailed = 0; $sSkipped = 0; $sNote = ''
$sReads = New-Object System.Collections.Generic.List[string]
function Quote-Arg([string]$a) {   # one argument for the shell line: cmd.exe on Windows, /bin/sh elsewhere
    if ($onWindows) { return '"' + $a + '"' }
    return "'" + ($a -replace "'", "'\''") + "'"
}
function Find-Interpreter([string]$kind) {   # the program and its leading arguments (none when not found): the caller collects them with @()
    if ($kind -eq 'python3') {
        if ($env:PYTHON) { return $env:PYTHON }
        foreach ($c in @('py', 'python3', 'python')) {
            $cmd = Get-Command $c -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($cmd) { if ($c -eq 'py') { return @($cmd.Source, '-3') } else { return $cmd.Source } }
        }
        return @()
    }
    $cmd = Get-Command $kind -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cmd) { return $cmd.Source }
    return @()
}
if (-not (Test-Path -LiteralPath $scriptsFile -PathType Leaf)) {
    $sNote = 'script tests: none run (no host_scripts.tsv beside host_tests.tsv: a tree configured before dm/host/CMakeLists.txt wrote it, or a copy of the build without it)'
    Write-Host $sNote
} else {
    foreach ($l in @(Get-Content -LiteralPath $scriptsFile -Encoding UTF8)) {
        if (-not $l -or $l.StartsWith('#')) { continue }
        $f = $l.Split("`t")
        if ($f.Count -lt 7) { continue }
        $name = $f[0]; $interp = $f[1]; $scriptRel = $f[2]; $sArgs = $f[3]; $sEnv = $f[4]; $reads = $f[5]; $skip = $f[6]
        $sTotal++
        $log = Join-Path $LogDir "$name.log"
        $scriptPath = Join-Path $repo ($scriptRel.Replace('/', [IO.Path]::DirectorySeparatorChar))
        $argList = @()
        if ($sArgs -ne '-') {
            foreach ($a in ($sArgs -split ' ')) {
                if ($a -eq '') { continue }
                if ($a -eq '""') { $a = '' }
                $argList += $a.Replace('{build}', $BuildDir).Replace('{repo}', $repo)
            }
        }
        $envPairs = @()
        if ($sEnv -ne '-') { foreach ($kv in ($sEnv -split ' ')) { if ($kv -match '^([A-Za-z_][A-Za-z0-9_]*)=(.*)$') { $envPairs += ,@($Matches[1], $Matches[2].Replace('{build}', $BuildDir).Replace('{repo}', $repo)) } } }
        $missing = @()
        if ($reads -ne '-') {
            foreach ($v in ($reads -split ' ')) {
                if ($v -eq '') { continue }
                $val = [Environment]::GetEnvironmentVariable($v)
                if ($v -eq 'NXAOT_PACKS' -and $Packs) { $val = $Packs; $envPairs += ,@('NXAOT_PACKS', $Packs) }   # the packs folder of the AOT tests (-Packs, build\packs\win64)
                if ($val) { $note = "$v=$val" } else { $note = "$v not set"; $missing += $v }
                if (-not $sReads.Contains($note)) { $sReads.Add($note) }
            }
        }
        $interpreter = @(Find-Interpreter $interp)
        $quoted = ''
        foreach ($a in $argList) { $quoted += ' ' + (Quote-Arg $a) }
        $envText = ''
        foreach ($pair in $envPairs) { if ($onWindows) { $envText += "set $($pair[0])=$($pair[1])&& " } else { $envText += "$($pair[0])=$($pair[1]) " } }
        if ($interpreter.Count -eq 0) {
            [System.IO.File]::WriteAllText($log, "date: $(Now)`ncommit: $commit`ncommand: (none: no $interp interpreter found on PATH)`n`n`nexit: none`n", $utf8)
            $line = "FAIL $name (no $interp interpreter found on PATH; see $name.log)"; $sFailed++
            Write-Host $line; $results.Add($line); continue
        }
        $progText = (Quote-Arg $interpreter[0])
        foreach ($a in ($interpreter | Select-Object -Skip 1)) { $progText += ' ' + $a }
        $progText += ' ' + (Quote-Arg $scriptPath) + $quoted
        if ($onWindows) {
            $cmdline = "cd /d `"$BuildDir`" && $envText$progText"
            $inner = "$progText >> `"$log`" 2>&1"
        } else {
            $cmdline = "cd $BuildDir && $envText$progText"
            $inner = "$progText >> '$log' 2>&1"
        }
        [System.IO.File]::WriteAllText($log, "date: $(Now)`ncommit: $commit`ncommand: $cmdline`n`n", $utf8)
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.UseShellExecute = $false
        $psi.WorkingDirectory = $BuildDir
        foreach ($pair in $envPairs) {
            $psi.EnvironmentVariables[$pair[0]] = $pair[1]
            if ($pair[0] -eq 'TMPDIR') { New-Item -ItemType Directory -Force -Path $pair[1] | Out-Null }
        }
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
        $hex = '0x{0:X8}' -f $rc
        # The script's own skip line (the last output line that says "skip", else its last line), for the summary
        $out = @(Get-Content -LiteralPath $log -Encoding UTF8 | Select-Object -Skip 4)
        $said = @($out | Where-Object { $_ -match 'skip' } | Select-Object -Last 1)
        if ($said.Count -eq 0) { $said = @($out | Where-Object { $_.Trim() } | Select-Object -Last 1) }
        $saidText = ''; if ($said.Count -gt 0) { $saidText = [string]$said[0] }
        [System.IO.File]::AppendAllText($log, "`nexit: $rc ($hex, $secs s)$(if ($timedOut) { ', killed after the time limit' })`n", $utf8)
        if ($timedOut) { $line = "TIMEOUT $name (killed after $TimeoutSeconds s; see $name.log)"; $sFailed++ }
        elseif ($rc -eq 0) { $line = "PASS $name ($secs s)"; $sPassed++ }
        elseif ($skip -ne '-' -and $rc -eq [int]$skip) {
            $why = ''; foreach ($v in $missing) { $why += "; $v not set" }
            $line = "SKIP $name (exit $rc, $secs s$why): `"$saidText`""; $sSkipped++
        }
        else { $line = "FAIL $name (exit $rc = $hex, $secs s; see $name.log)"; $sFailed++ }
        Write-Host $line
        $results.Add($line)
    }
}

# The port's prebuilt tools: each test module through Python's unittest from the repository root, with the program in its variable; PASS when unittest exits 0 (skips included)
if ($PortTools) {
    $exeSuffix = ''; if ($onWindows) { $exeSuffix = '.exe' }
    $py = @(Find-Interpreter 'python3')
    $toolTests = @(@('port_nx_extract', 'NX_EXTRACT', "nx_extract$exeSuffix", 'dm/tools/tests/test_nx_extract.py'),
                   @('port_dmsh_xlate_vk', 'XLATE_VK', "dmsh_xlate_vk$exeSuffix", 'dm/gfx/tests/test_dmsh_vk.py'))
    foreach ($t in $toolTests) {
        $name = $t[0]; $var = $t[1]; $prog = Join-Path $PortTools $t[2]; $test = $t[3]
        $sTotal++
        $log = Join-Path $LogDir "$name.log"
        $why = ''
        if ($py.Count -eq 0) { $why = 'no python3 interpreter found on PATH' } elseif (-not (Test-Path -LiteralPath $prog -PathType Leaf)) { $why = "no $prog" }
        if ($why) {
            [System.IO.File]::WriteAllText($log, "date: $(Now)`ncommit: $commit`ncommand: (none: $why)`n`n`nexit: none`n", $utf8)
            $line = "FAIL $name ($why; see $name.log)"; $sFailed++
            Write-Host $line; $results.Add($line); continue
        }
        $progText = (Quote-Arg $py[0])
        foreach ($a in ($py | Select-Object -Skip 1)) { $progText += ' ' + $a }
        $progText += " -m unittest -v $test"
        if ($onWindows) {
            $cmdline = "cd /d `"$repo`" && set $var=$prog&& $progText"
            $inner = "$progText >> `"$log`" 2>&1"
        } else {
            $cmdline = "cd $repo && $var=$prog $progText"
            $inner = "$progText >> '$log' 2>&1"
        }
        [System.IO.File]::WriteAllText($log, "date: $(Now)`ncommit: $commit`ncommand: $cmdline`n`n", $utf8)
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.UseShellExecute = $false
        $psi.WorkingDirectory = $repo
        $psi.EnvironmentVariables[$var] = $prog
        $psi.EnvironmentVariables['PYTHONDONTWRITEBYTECODE'] = '1'   # no __pycache__ in the checkout
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
        $hex = '0x{0:X8}' -f $rc
        [System.IO.File]::AppendAllText($log, "`nexit: $rc ($hex, $secs s)$(if ($timedOut) { ', killed after the time limit' })`n", $utf8)
        if ($timedOut) { $line = "TIMEOUT $name (killed after $TimeoutSeconds s; see $name.log)"; $sFailed++ }
        elseif ($rc -eq 0) { $line = "PASS $name ($secs s)"; $sPassed++ }
        else { $line = "FAIL $name (exit $rc = $hex, $secs s; see $name.log)"; $sFailed++ }
        Write-Host $line
        $results.Add($line)
    }
}

$totalLine = "$total runs: $passed passed, $failed failed"
$scriptsLine = "$sTotal script tests: $sPassed passed, $sFailed failed, $sSkipped skipped"
$os = [Environment]::OSVersion.VersionString
if ($onWindows) { try { $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'; $os = "$($cv.ProductName) $($cv.DisplayVersion) build $($cv.CurrentBuild).$($cv.UBR)" } catch { } }
$sb = New-Object System.Text.StringBuilder
[void]$sb.Append("date: $started`ncommit: $commit`ncommand: $invoked`ndirectory: $startedIn`nhost: $os`nbuild folder: $BuildDir`n")
[void]$sb.Append("PowerShell: $($PSVersionTable.PSVersion)`n")
if ($Packs) { [void]$sb.Append("packs: $Packs`n") } else { [void]$sb.Append("packs: none (the AOT tests ran without packs)`n") }
if ($PortTools) { [void]$sb.Append("port tools: $PortTools`n") } else { [void]$sb.Append("port tools: none (-PortTools, or build\port_tools\win-x64 from dm/tools/publish_port_tools.sh)`n") }
if ($sReads.Count -gt 0) { [void]$sb.Append("script tests' environment: $($sReads -join ' ')`n") }
foreach ($l in ($lines | Select-Object -Skip 1)) { if ($l.StartsWith('# ')) { [void]$sb.Append($l.Substring(2) + "`n") } }
[void]$sb.Append("`n")
foreach ($r in $results) { [void]$sb.Append("$r`n") }
if ($sNote) { [void]$sb.Append("$sNote`n") }
[void]$sb.Append("`n$totalLine`n$scriptsLine`n")
[System.IO.File]::WriteAllText((Join-Path $LogDir 'summary.txt'), $sb.ToString(), $utf8)
Write-Host "$totalLine; $scriptsLine (logs in $LogDir)"
if ($total -gt 0 -and $failed -eq 0 -and $sFailed -eq 0) { exit 0 } else { exit 1 }
