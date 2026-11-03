# Verify consistency of this complete package; this is not a publisher signature.
[CmdletBinding()]
param([string]$Directory = $PSScriptRoot)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-RegularEntry([string]$Path) {
    $entry = Get-Item -LiteralPath $Path -Force
    if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Reparse paths are not package files: $Path"
    }
    return $entry
}

try {
    $root = Get-RegularEntry $Directory
    if (-not $root.PSIsContainer) { throw 'Package directory required.' }
    $manifestFile = Get-RegularEntry (Join-Path $root.FullName 'manifest.json')
    if ($manifestFile.PSIsContainer -or $manifestFile.Length -gt 131072) { throw 'Invalid package manifest.' }
    $manifest = Get-Content -LiteralPath $manifestFile.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($manifest.schema -cne 'LMB-NATIVE-CANDIDATE-1' -or $manifest.system -cne 'Windows' -or
        $manifest.architecture -cne 'x86_64') { throw 'Not a Windows x86-64 package manifest.' }
    $required = @('Lumabri.cmd','Verify-Lumabri.ps1','START_HERE.md',
        'bin/lumabri.exe','bin/tracker.exe','bin/maintainer.exe','bin/segment_node.exe','bin/segment_chat.exe',
        'licenses/Lumabri-LICENSE','licenses/Colibri-LICENSE','licenses/Windows-runtime-NOTICES.txt')
    $optional = @('licenses/Colibri-NOTICE','licenses/Colibri-THIRD_PARTY_NOTICES.md')
    $declared = [Collections.Generic.Dictionary[string,object]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($property in $manifest.files.PSObject.Properties) {
        $name = $property.Name
        if (($required -cnotcontains $name -and $optional -cnotcontains $name) -or $declared.ContainsKey($name)) {
            throw "Unexpected package path: $name"
        }
        if ($property.Value.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
            $property.Value.bytes -isnot [long] -and $property.Value.bytes -isnot [int] -or
            $property.Value.bytes -lt 0) { throw "Invalid file digest/length: $name" }
        $declared.Add($name,$property.Value)
    }
    foreach ($name in $required) { if (-not $declared.ContainsKey($name)) { throw "Missing declaration: $name" } }
    $found = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $pending = [Collections.Generic.Stack[string]]::new()
    $pending.Push($root.FullName)
    $entries = 0
    while ($pending.Count) {
        $parent = $pending.Pop()
        foreach ($entry in Get-ChildItem -LiteralPath $parent -Force) {
            $entries++
            if ($entries -gt 64 -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw 'Unexpected entry or reparse point in package.'
            }
            if ($entry.PSIsContainer) { $pending.Push($entry.FullName); continue }
            $relative = $entry.FullName.Substring($root.FullName.TrimEnd('\').Length + 1).Replace('\','/')
            if ($relative -ceq 'manifest.json') { continue }
            if (-not $declared.ContainsKey($relative) -or -not $found.Add($relative)) {
                throw "Extra or duplicate package file: $relative"
            }
            $expected = $declared[$relative]
            if ($entry.Length -ne $expected.bytes -or
                (Get-FileHash -LiteralPath $entry.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -cne $expected.sha256) {
                throw "Package file changed: $relative"
            }
        }
    }
    if ($found.Count -ne $declared.Count) { throw 'Package has missing files.' }
    Write-Output 'Windows package contents and hashes: PASS'
    exit 0
} catch {
    Write-Error "Package verification failed: $($_.Exception.Message)"
    exit 1
}
