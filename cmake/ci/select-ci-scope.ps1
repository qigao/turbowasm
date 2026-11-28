param(
    [Parameter(Mandatory)][ValidateSet('pull_request', 'push', 'workflow_dispatch')]
    [string]$EventName,
    [AllowEmptyString()][string]$BaseRef,
    [Parameter(Mandatory)][string]$HeadRef,
    [bool]$SkipWindows = $false
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# PRs qualify the whole proposed change, not only the last pushed commit.
# Manual runs always build every applicable platform.
$selected = $EventName -eq 'workflow_dispatch'
if (-not $selected) {
    if ([string]::IsNullOrWhiteSpace($BaseRef)) { throw "Missing comparison base for $EventName" }
    if ($EventName -eq 'push' -and $BaseRef -match '^0+$') {
        $changed = @(git ls-tree -r --name-only $HeadRef)
    } else {
        $range = if ($EventName -eq 'pull_request') { "$BaseRef...$HeadRef" } else { "$BaseRef..$HeadRef" }
        $changed = @(git diff --name-only --no-renames $range --)
    }
    if ($LASTEXITCODE -ne 0) { throw "Cannot determine changed files for $EventName" }
    # Runtime, Component and their wasm/tests share implementation across all hosts.
    # Only prose changes can omit the native graph; other changes qualify all eight.
    $selected = @($changed | Where-Object { $_ -notmatch '\.(md|rst)$' }).Count -gt 0
}

# The platform and toolchain contract for native CI.
$profiles = @(Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'native-matrix.json') | ConvertFrom-Json -AsHashtable)
$builds = @()
if ($selected) {
    foreach ($profileEntry in $profiles) {
        if ($SkipWindows -and $profileEntry.family -eq 'windows') { continue }
        $builds += $profileEntry
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
        Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Value "| $($entry.id) | $($entry.preset) | $($entry.sdk) |"
    }
    if ($builds.Count -eq 0) {
        Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Value 'Prose-only change: no native build selected.'
    }
}
