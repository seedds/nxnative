# Removes what setup.ps1 added. Your build folder's own files stay, apart from the Python copy made for the account.
#Requires -RunAsAdministrator
$User = 'nxbuild'; $Build = 'C:\dmb'; $Data = "$env:ProgramData\ssh"; $Config = "$Data\sshd_config"; $Backup = "$Config.before-nxnative"; $Marker = "$env:ProgramData\nxnative-ssh.json"
$ErrorActionPreference = 'Continue'
Stop-Service sshd -ErrorAction SilentlyContinue
Remove-NetFirewallRule -Name 'nxnative-ssh' -ErrorAction SilentlyContinue
Remove-Item "$Data\nxbuild_authorized_keys" -Force -ErrorAction SilentlyContinue
if (Test-Path $Backup) { Move-Item $Backup $Config -Force }
$installed = $true
if (Test-Path $Marker) { $installed = (Get-Content $Marker -Raw | ConvertFrom-Json).installed_by_this_script }
if ($installed) {
    Set-Service sshd -StartupType Disabled -ErrorAction SilentlyContinue
    & sc.exe delete sshd | Out-Null   # a service the setup registered itself; removing the capability does not always remove it
    Get-WindowsCapability -Online -Name 'OpenSSH.Server*' | Where-Object State -eq 'Installed' | ForEach-Object { Remove-WindowsCapability -Online -Name $_.Name | Out-Null }
} else {
    Get-NetFirewallRule -Name 'OpenSSH-Server-In-TCP' -ErrorAction SilentlyContinue | Enable-NetFirewallRule
    Start-Service sshd -ErrorAction SilentlyContinue
}
$u = Get-LocalUser -Name $User -ErrorAction SilentlyContinue
if ($u) {
    Get-CimInstance Win32_UserProfile | Where-Object { $_.SID -eq $u.SID.Value } | Remove-CimInstance -ErrorAction SilentlyContinue
    Remove-LocalUser -Name $User
}
Remove-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon\SpecialAccounts\UserList' -Name $User -ErrorAction SilentlyContinue
if (Test-Path "$Build\python") { Remove-Item "$Build\python" -Recurse -Force }
if (Test-Path $Build) { icacls $Build /remove $User /T /C /Q | Out-Null }
Remove-Item $Marker -Force -ErrorAction SilentlyContinue
Write-Host ''; Write-Host 'Removed. A network you changed to Private stays Private: change it back in Windows Settings if you want to.' -ForegroundColor Green
