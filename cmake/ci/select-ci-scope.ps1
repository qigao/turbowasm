param(
    [Parameter(Mandatory)][ValidateSet('pull_request', 'push', 'workflow_dispatch')]
    [string]$EventName,
    [AllowEmptyString()][string]$BaseRef,
    [Parameter(Mandatory)][string]$HeadRef,
    [ValidateSet('ci', 'sdk')][string]$Profile = 'ci',
    [bool]$SkipWindows = $false
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# PRs qualify the whole proposed change, not only the last pushed commit.
# Manual runs and SDK preparation always build every applicable platform.
$selected = $EventName -eq 'workflow_dispatch' -or $Profile -eq 'sdk'
if (-not $selected) {
    if ([string]::IsNullOrWhiteSpace($BaseRef)) { throw "Missing comparison base for $EventName" }
    if ($EventName -eq 'push' -and $BaseRef -match '^0+$') {
        $changed = @(git ls-tree -r --name-only $HeadRef)
    } else {
        $range = if ($EventName -eq 'pull_request') { "$BaseRef...$HeadRef" } else { "$BaseRef..$HeadRef" }
        $changed = @(git diff --name-only --no-renames $range --)
    }
    if ($LASTEXITCODE -ne 0) { throw "Cannot determine changed files for $EventName" }
    # Runtime, Component and their tests share implementation across all hosts.
    # Only prose changes can omit the native graph; other changes qualify all five.
    $selected = @($changed | Where-Object { $_ -notmatch '\.(md|rst)$' }).Count -gt 0
}

# The platform and toolchain contract is shared by CI and SDK packaging.
$profiles = @(
    @{ id = 'linux-release'; family = 'linux'; runner = 'ubuntu-24.04'; rid = 'linux-x64'; triplet = 'x64-linux'; host_triplet = 'x64-linux'; preset = 'ci-linux-user'; sdk_preset = 'ci-sdk-user'; consumer = 'ci-user'; tests = $true; littlefs = $true },
    @{ id = 'windows-release'; family = 'windows'; runner = 'windows-2025'; rid = 'windows-x64'; triplet = 'x64-windows'; host_triplet = 'x64-windows'; preset = 'ci-win-user'; sdk_preset = 'ci-win-sdk-user'; consumer = 'ci-win-user'; tests = $true },
    @{ id = 'android-arm64-v8a-release'; family = 'android'; runner = 'ubuntu-24.04'; rid = 'android-arm64-v8a'; triplet = 'arm64-android'; host_triplet = 'x64-linux'; preset = 'ci-android-user'; sdk_preset = 'ci-android-sdk-user'; consumer = 'ci-android-user' },
    @{ id = 'linux-mir-release'; family = 'linux'; runner = 'ubuntu-24.04'; rid = 'linux-x64'; triplet = 'x64-linux'; host_triplet = 'x64-linux'; preset = 'ci-mir-user'; tests = $true },
    @{ id = 'macos-mir-release'; sdk_id = 'macos-release'; family = 'mac'; runner = 'macos-15'; rid = 'macos-arm64'; triplet = 'arm64-osx'; host_triplet = 'arm64-osx'; preset = 'ci-macos-mir-user'; sdk_preset = 'ci-sdk-user'; tests = $true }
)
$builds = @()
if ($selected) {
    foreach ($profileEntry in $profiles) {
        if ($SkipWindows -and $profileEntry.family -eq 'windows') { continue }
        if ($Profile -eq 'sdk' -and -not $profileEntry.ContainsKey('sdk_preset')) { continue }
        $entry = $profileEntry.Clone()
        if ($Profile -eq 'ci' -and $env:CI_METALLIC_GUESTS -eq 'true') {
            if ($entry.id -eq 'linux-release') {
                $entry.preset = 'ci-metallic-user'
                $entry.metallic = $true
            } elseif ($entry.id -eq 'macos-mir-release') {
                $entry.preset = 'ci-macos-metallic-user'
                $entry.consumer = 'ci-macos-user'
                $entry.metallic = $true
            }
        }
        if ($Profile -eq 'sdk') {
            $entry.preset = $entry.sdk_preset
            if ($entry.ContainsKey('sdk_id')) { $entry.id = $entry.sdk_id }
            foreach ($key in @('consumer', 'tests', 'littlefs')) { $entry.Remove($key) }
            $entry.package = $true
        }
        $entry.Remove('sdk_preset'); $entry.Remove('sdk_id')
        $builds += $entry
    }
}
$matrix = ConvertTo-Json -InputObject @{ include = $builds } -Depth 5 -Compress
Write-Output $matrix
if ($env:GITHUB_OUTPUT) {
    Add-Content -LiteralPath $env:GITHUB_OUTPUT -Value "builds=$matrix"
    Add-Content -LiteralPath $env:GITHUB_OUTPUT -Value "has_builds=$($builds.Count -gt 0)".ToLowerInvariant()
}
if ($env:GITHUB_STEP_SUMMARY) {
    if ($SkipWindows) {
        Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Value 'Windows qualification explicitly omitted for this run.'
    }
    Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Value "| Profile | Preset | SDK |`n|---|---|---|"
    foreach ($entry in $builds) {
        Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Value "| $($entry.id) | $($entry.preset) | $($entry.rid) |"
    }
    if ($builds.Count -eq 0) {
        Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Value 'Prose-only change: no native build selected.'
    }
}
