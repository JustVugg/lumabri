# Execute the helper, not just its outer -WhatIf. No real firewall cmdlet is
# invoked. Mandatory parameters match the production command contracts.
$ErrorActionPreference = 'Stop'
$script:rules = @{}
$script:created = 0
$script:updated = 0
$script:removed = 0
function Get-NetFirewallRule { [CmdletBinding()] param($Name) $script:rules[$Name] }
function Get-NetFirewallHyperVRule { [CmdletBinding()] param($Name) $script:rules[$Name] }
function New-NetFirewallRule {
    [CmdletBinding()] param([Parameter(Mandatory)]$Name, [Parameter(Mandatory)]$DisplayName,
        $Group, $Direction, $Action, $Protocol, $LocalPort, $RemoteAddress, $Profile)
    if ($RemoteAddress -ne '192.168.1.0/24' -or $Action -ne 'Allow' -or $Direction -ne 'Inbound') { throw 'Unsafe Windows rule' }
    $script:rules[$Name] = $true; $script:created++
}
function New-NetFirewallHyperVRule {
    [CmdletBinding()] param([Parameter(Mandatory)]$Name, [Parameter(Mandatory)]$DisplayName,
        [Parameter(Mandatory)]$VMCreatorId, $Direction, $Action, $Protocol, $LocalPorts, $RemoteAddresses)
    if ($VMCreatorId -ne '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}' -or
        $RemoteAddresses -ne '192.168.1.0/24' -or $Action -ne 'Allow') { throw 'Unsafe Hyper-V rule' }
    $expected = if ($Protocol -eq 'TCP') { '47300-47315' } else { '47300' }
    if ($LocalPorts -ne $expected) { throw 'Incorrect ports' }
    $script:rules[$Name] = $true; $script:created++
}
function Set-NetFirewallRule {
    [CmdletBinding()] param($Name, $Enabled, $Direction, $Action, $Protocol, $LocalPort, $RemoteAddress, $Profile)
    if (-not $script:rules[$Name]) { throw 'Updating an absent rule' }; $script:updated++
}
function Set-NetFirewallHyperVRule {
    [CmdletBinding()] param($Name, $Enabled, $Direction, $Action, $Protocol, $LocalPorts, $RemoteAddresses)
    if (-not $script:rules[$Name]) { throw 'Updating an absent Hyper-V rule' }; $script:updated++
}
function Remove-NetFirewallRule { [CmdletBinding()] param($Name) $script:rules.Remove($Name); $script:removed++ }
function Remove-NetFirewallHyperVRule { [CmdletBinding()] param($Name) $script:rules.Remove($Name); $script:removed++ }

$helper = Join-Path $PSScriptRoot '../../tools/setup-household-firewall.ps1'
# Refuse to run an incomplete invocation rather than waiting for a mandatory
# parameter prompt on the CI runner (the original user-visible failure).
$tokens = $null; $errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile((Resolve-Path $helper), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Helper parse errors' }
$commands = $ast.FindAll({ param($node)
    $node -is [System.Management.Automation.Language.CommandAst] -and
    $node.GetCommandName() -eq 'New-NetFirewallHyperVRule'
}, $true)
foreach ($command in $commands) {
    $parameters = $command.CommandElements | Where-Object { $_ -is [System.Management.Automation.Language.CommandParameterAst] }
    if ('DisplayName' -notin $parameters.ParameterName) { throw 'Missing mandatory DisplayName' }
}
# Hosted Windows runners are administrators. Refuse elevation in this test.
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this test on the Windows CI runner; no elevation is performed' }
& $helper -Subnet 192.168.1.0/24 -Confirm:$false
if ($script:created -ne 4 -or $script:rules.Count -ne 4) { throw 'Creation did not exercise all four commands' }
& $helper -Subnet 192.168.1.0/24 -Confirm:$false
if ($script:created -ne 4 -or $script:updated -ne 4) { throw 'Update is not idempotent' }
& $helper -Subnet 192.168.1.0/24 -Remove -Confirm:$false
if ($script:removed -ne 4 -or $script:rules.Count) { throw 'Removal failed' }
Write-Host 'FIREWALL HELPER: PASS (mandatory display names, scoped create, update, remove)'
