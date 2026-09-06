# Invoked only by an explicit /network approval. Never disables a firewall.
[CmdletBinding(SupportsShouldProcess=$true, ConfirmImpact='Medium')]
param(
    [Parameter(Mandatory=$true)][string]$Subnet,
    [ValidateRange(1024,65520)][int]$PortBase = 47300,
    [switch]$Remove
)
$ErrorActionPreference = 'Stop'
if ($Subnet -notmatch '^([0-9]{1,3}\.){3}[0-9]{1,3}/([0-9]{1,2})$') { throw 'Invalid LAN subnet' }
$parts = $Subnet.Split('/')
$address = [System.Net.IPAddress]::Parse($parts[0])
$octets = $address.GetAddressBytes()
$prefix = [int]$parts[1]
$private = $octets.Length -eq 4 -and ($octets[0] -eq 10 -or
    ($octets[0] -eq 172 -and $octets[1] -ge 16 -and $octets[1] -le 31) -or
    ($octets[0] -eq 192 -and $octets[1] -eq 168))
$minimumPrefix = if ($octets[0] -eq 10) { 8 } elseif ($octets[0] -eq 172) { 12 } else { 16 }
if (-not $private -or $prefix -lt $minimumPrefix -or $prefix -gt 30) { throw 'Only a private LAN subnet is allowed' }
if (-not $PSCmdlet.ShouldProcess($Subnet, $(if ($Remove) { 'Remove Lumabri household firewall rules' } else { "Allow household TCP $PortBase-$($PortBase + 15) and UDP $PortBase" }))) { return }
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"{0}"' -f $PSCommandPath),
                   '-Subnet', $Subnet, '-PortBase', $PortBase)
    if ($Remove) { $arguments += '-Remove' }
    $elevated = Start-Process powershell.exe -Verb RunAs -ArgumentList $arguments -Wait -PassThru
    exit $elevated.ExitCode
}
$creator = '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}'
if (-not (Get-Command New-NetFirewallHyperVRule -ErrorAction SilentlyContinue)) {
    throw 'The WSL Hyper-V firewall commands are unavailable. No firewall rules were changed.'
}
foreach ($protocol in @('TCP', 'UDP')) {
    $name = 'Lumabri-Household-' + $protocol
    $hyperName = $name + '-WSL'
    $windows = Get-NetFirewallRule -Name $name -ErrorAction SilentlyContinue
    $hyper = Get-NetFirewallHyperVRule -Name $hyperName -ErrorAction SilentlyContinue
    if ($Remove) {
        if ($windows) { Remove-NetFirewallRule -Name $name }
        if ($hyper) { Remove-NetFirewallHyperVRule -Name $hyperName }
        continue
    }
    $ports = if ($protocol -eq 'TCP') { '{0}-{1}' -f $PortBase, ($PortBase + 15) } else { "$PortBase" }
    if ($windows) {
        Set-NetFirewallRule -Name $name -Enabled True -Direction Inbound -Action Allow `
            -Protocol $protocol -LocalPort $ports -RemoteAddress $Subnet -Profile Any | Out-Null
    } else {
        New-NetFirewallRule -Name $name -DisplayName $name -Group 'Lumabri household' `
            -Direction Inbound -Action Allow -Protocol $protocol -LocalPort $ports `
            -RemoteAddress $Subnet -Profile Any | Out-Null
    }
    if ($hyper) {
        Set-NetFirewallHyperVRule -Name $hyperName -Enabled True -Direction Inbound -Action Allow `
            -Protocol $protocol -LocalPorts $ports -RemoteAddresses $Subnet | Out-Null
    } else {
        New-NetFirewallHyperVRule -Name $hyperName -DisplayName $hyperName -VMCreatorId $creator `
            -Direction Inbound -Action Allow -Protocol $protocol -LocalPorts $ports `
            -RemoteAddresses $Subnet | Out-Null
    }
}
if ($Remove) { Write-Host 'Lumabri household rules removed.' }
else { Write-Host "Lumabri allowed from $Subnet only: TCP $PortBase-$($PortBase + 15), UDP $PortBase." }
