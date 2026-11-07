# Lets this project's Mac run build commands on this PC over SSH, with a limited account and a key only. Undo: UNDO.bat
#Requires -RunAsAdministrator
$ErrorActionPreference = 'Stop'
$PublicKey = (Get-Content (Join-Path $PSScriptRoot 'mac_key.pub') -Raw).Trim()   # the public half of the Mac's key: ssh-keygen -t ed25519 -N '' -f ~/.ssh/nxnative_laptop, then copy the .pub here as mac_key.pub
$User = 'nxbuild'; $Build = 'C:\dmb'; $Data = "$env:ProgramData\ssh"; $Config = "$Data\sshd_config"; $Backup = "$Config.before-nxnative"; $Keys = "$Data\nxbuild_authorized_keys"; $Marker = "$env:ProgramData\nxnative-ssh.json"
Start-Transcript -Path (Join-Path $PSScriptRoot 'setup_log.txt') -Force | Out-Null
function Step($t) { Write-Host ''; Write-Host $t -ForegroundColor Cyan }
try {
    Step '1 of 6: OpenSSH Server (this can take a few minutes)'
    $cap = Get-WindowsCapability -Online -Name 'OpenSSH.Server*' | Select-Object -First 1
    $we = $cap.State -ne 'Installed'
    if ($we) { $r = Add-WindowsCapability -Online -Name $cap.Name; Write-Host "Install finished; restart needed: $($r.RestartNeeded)" } else { Write-Host 'Already installed.' }
    # Windows can report this before the service exists: find out which case this is
    $state = (Get-WindowsCapability -Online -Name 'OpenSSH.Server*' | Select-Object -First 1).State
    $exe = "$env:WINDIR\System32\OpenSSH\sshd.exe"; $pending = Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending'
    Write-Host "State: $state; program present: $(Test-Path $exe); service present: $([bool](Get-Service sshd -ErrorAction SilentlyContinue)); restart pending: $pending"
    if (-not (Get-Service sshd -ErrorAction SilentlyContinue)) {
        if ($pending -or $state -ne 'Installed' -or -not (Test-Path $exe)) { throw 'Windows has not finished installing OpenSSH Server. Restart the laptop, then run SETUP.bat as administrator again.' }
        New-Service -Name sshd -DisplayName 'OpenSSH SSH Server' -BinaryPathName "`"$exe`"" -StartupType Manual -Description 'SSH server, registered by the nxnative setup because Windows had not' | Out-Null
        Write-Host 'The program was there without its service, so the service was registered.'
    }
    if (-not (Test-Path $Marker)) { @{ installed_by_this_script = $we } | ConvertTo-Json | Set-Content $Marker }

    Step '2 of 6: the limited account'
    if (-not (Get-LocalUser -Name $User -ErrorAction SilentlyContinue)) {
        $b = New-Object byte[] 32; [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($b)
        $p = ConvertTo-SecureString ([Convert]::ToBase64String($b) + 'aZ9!') -AsPlainText -Force   # never shown or stored: the account is used through the key alone
        New-LocalUser -Name $User -Password $p -PasswordNeverExpires -UserMayNotChangePassword -Description 'Builds for the nxnative project over SSH' | Out-Null
    }
    Add-LocalGroupMember -SID 'S-1-5-32-545' -Member $User -ErrorAction SilentlyContinue   # the standard Users group, in any Windows language
    $hide = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon\SpecialAccounts\UserList'
    New-Item -Path $hide -Force | Out-Null; New-ItemProperty -Path $hide -Name $User -Value 0 -PropertyType DWord -Force | Out-Null   # not on the sign-in screen
    Write-Host "Account $User is ready: a standard account, hidden from the sign-in screen."

    Step '3 of 6: the build folder and Python'
    New-Item -ItemType Directory -Path $Build -Force | Out-Null
    icacls $Build /grant "${User}:(OI)(CI)M" /T /C /Q | Out-Null
    $py = @(Get-ChildItem 'C:\Users\*\AppData\Local\Programs\Python\Python314\python.exe', "$env:ProgramFiles\Python314\python.exe" -ErrorAction SilentlyContinue) | Select-Object -First 1
    if (-not $py) { throw 'Python 3.14 was not found. Install it from python.org, then run this again.' }
    robocopy $py.DirectoryName "$Build\python" /E /NFL /NDL /NJH /NJS /NP /XD __pycache__ | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Copying Python failed (robocopy $LASTEXITCODE)" }
    $global:LASTEXITCODE = 0
    icacls "$Build\python" /grant "${User}:(OI)(CI)M" /T /C /Q | Out-Null
    Write-Host "Copied Python to $Build\python, where the limited account can use it."

    Step '4 of 6: SSH settings: this key and this account only, no passwords'
    Start-Service sshd   # the first start writes the default settings
    Stop-Service sshd
    if (-not (Test-Path $Backup)) { Copy-Item $Config $Backup }
    $utf8 = New-Object Text.UTF8Encoding($false)
    $head = "# Added by nxnative setup; UNDO.bat restores the file`nPubkeyAuthentication yes`nPasswordAuthentication no`nKbdInteractiveAuthentication no`nAllowUsers $User`nAuthorizedKeysFile __PROGRAMDATA__/ssh/nxbuild_authorized_keys`n`n"
    [IO.File]::WriteAllText($Config, $head + [IO.File]::ReadAllText($Backup), $utf8)
    [IO.File]::WriteAllText($Keys, $PublicKey + "`n", $utf8)
    icacls $Keys /inheritance:r /grant '*S-1-5-18:F' /grant '*S-1-5-32-544:F' /grant "${User}:R" /Q | Out-Null
    & "$env:WINDIR\System32\OpenSSH\sshd.exe" -t
    if ($LASTEXITCODE -ne 0) { Copy-Item $Backup $Config -Force; throw 'The SSH settings did not pass their check, so the original settings were put back.' }

    Step '5 of 6: the firewall: home network only'
    Get-NetFirewallRule -Name 'OpenSSH-Server-In-TCP' -ErrorAction SilentlyContinue | Disable-NetFirewallRule   # it allows any address
    Remove-NetFirewallRule -Name 'nxnative-ssh' -ErrorAction SilentlyContinue
    New-NetFirewallRule -Name 'nxnative-ssh' -DisplayName 'nxnative SSH (local network, private networks only)' -Direction Inbound -Action Allow -Protocol TCP -LocalPort 22 -RemoteAddress LocalSubnet -Profile Private | Out-Null
    $public = @(Get-NetConnectionProfile | Where-Object { $_.NetworkCategory -eq 'Public' })
    foreach ($n in $public) {
        Write-Host "Windows treats the network '$($n.Name)' as Public, and the rule only opens on Private networks." -ForegroundColor Yellow
        if ((Read-Host "Is '$($n.Name)' your own home network, and should it become Private? (y/n)") -eq 'y') { Set-NetConnectionProfile -InterfaceIndex $n.InterfaceIndex -NetworkCategory Private; Write-Host 'Done.' }
        else { Write-Host 'Left as it is: the Mac cannot connect over this network.' -ForegroundColor Yellow }
    }

    Step '6 of 6: starting the service'
    Set-Service sshd -StartupType Automatic; Start-Service sshd
    if ((Get-Service sshd).Status -ne 'Running') { throw 'The SSH service did not start.' }
    $ip = @(Get-NetIPConfiguration | Where-Object { $_.IPv4DefaultGateway -and $_.NetAdapter.Status -eq 'Up' } | ForEach-Object { $_.IPv4Address.IPAddress })
    Write-Host ''; Write-Host 'Finished. Tell Claude these two lines:' -ForegroundColor Green
    Write-Host "  name: $env:COMPUTERNAME"
    Write-Host "  address: $($ip -join ', ')"
    Write-Host ''; Write-Host 'To remove all of this later, right-click UNDO.bat and choose Run as administrator.'
} catch {
    Write-Host ''; Write-Host "Setup stopped: $_" -ForegroundColor Red
    Write-Host "Send Claude the file setup_log.txt from this folder."
} finally { Stop-Transcript | Out-Null }
