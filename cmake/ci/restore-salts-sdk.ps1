param(
  [string]$SaltsRid = "",
  [switch]$Local
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not $Local) {
  foreach ($name in @("RUNNER_TEMP", "GITHUB_ENV", "GITHUB_WORKSPACE")) {
    if ([string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($name))) {
      throw "$name is required for native curl-based CI SDK restore"
    }
  }
}
if ([string]::IsNullOrWhiteSpace($SaltsRid)) {
  if ($Local) { throw "Local restore requires an explicit -SaltsRid" }
  switch ("$env:RUNNER_OS/$env:RUNNER_ARCH") {
    "Linux/X64" { $SaltsRid = "linux-x64" }
    "Windows/X64" { $SaltsRid = "windows-x64" }
    "macOS/X64" { $SaltsRid = "macos-x64" }
    "macOS/ARM64" { $SaltsRid = "macos-arm64" }
    default { throw "Unsupported Salts SDK host: $env:RUNNER_OS/$env:RUNNER_ARCH" }
  }
}
if ($SaltsRid -notin @("linux-x64", "windows-x64", "macos-x64", "macos-arm64", "android-arm64-v8a")) {
  throw "Unsupported Salts.Native RID: $SaltsRid"
}
if ($env:SALTS_SDK_VERSION) {
  throw "SALTS_SDK_VERSION override is forbidden; the audited selector is 2.3.0-*"
}

$token = if ($env:GH_TOKEN) { $env:GH_TOKEN } else { $env:GITHUB_TOKEN }
if ([string]::IsNullOrWhiteSpace($token)) {
  throw "GitHub Packages requires GH_TOKEN or GITHUB_TOKEN with read:packages"
}
$env:GH_TOKEN = $token

$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../.."))
$cacheCheckout = Join-Path $root "build/vcpkg-cache-contract"
if (-not (Test-Path -LiteralPath (Join-Path $cacheCheckout 'scripts/github-packages.py'))) {
  if (-not $Local -or -not $env:VCPKG_CACHE_REPOSITORY_ROOT) {
    throw "Canonical qigao/vcpkg-cache curl package client checkout is missing"
  }
  $cacheCheckout = $env:VCPKG_CACHE_REPOSITORY_ROOT
}
$client = Join-Path $cacheCheckout "scripts/github-packages.py"
if (-not (Test-Path -LiteralPath $client -PathType Leaf)) {
  throw "Missing pinned shared GitHub Packages HTTP client: $client"
}

$restoreRoot = if ($Local) { Join-Path $root "build/native-sdk" } else { $env:RUNNER_TEMP }
$packages = Join-Path $restoreRoot "github-packages"
$archive = Join-Path $restoreRoot "salts-native.nupkg"
$versionFile = Join-Path $restoreRoot "salts-native.version"
New-Item -ItemType Directory -Path $restoreRoot -Force | Out-Null

# The shared NuGet V3 client performs authenticated curl GET and validates
# the exact NuGet ID/version/zip archive, without invoking dotnet or Mono.
& python $client floating 'Salts.Native' '2.3.0-*' $archive $versionFile
if ($LASTEXITCODE -ne 0) { throw "GitHub Packages curl metadata/package download failed" }
if (-not (Test-Path -LiteralPath $versionFile -PathType Leaf)) {
  throw "Missing exact downloaded Salts.Native version provenance"
}
$version = (Get-Content -LiteralPath $versionFile -Raw).Trim()
if ($version -notmatch '^2[.]3[.]0-[0-9A-Za-z][0-9A-Za-z.-]*$') {
  throw "Unexpected GitHub Packages Salts.Native version: $version"
}
$packageRoot = Join-Path $packages "salts.native/$version"
if (Test-Path -LiteralPath $packageRoot) {
  Remove-Item -LiteralPath $packageRoot -Recurse -Force
}
& python $client extract $archive $packageRoot
if ($LASTEXITCODE -ne 0) { throw "Validated Salts.Native package extraction failed" }

$saltsRoot = Join-Path $packageRoot "sdk/$SaltsRid"
foreach ($rel in @(
  'lib/cmake/Salts/SaltsConfig.cmake',
  'lib/cmake/Salts/SaltsTargets.cmake',
  'include/cmeta/function.h'
)) {
  if (-not (Test-Path -LiteralPath (Join-Path $saltsRoot $rel) -PathType Leaf)) {
    throw "Downloaded Salts.Native $version lacks required SDK file: $rel"
  }
}
$env:SALTS_ROOT = $saltsRoot
$env:SALTS_VERSION = $version
$env:QIGAO_GITHUB_PACKAGES_ROOT = $packages
if (-not $Local) {
  "SALTS_ROOT=$saltsRoot" >> $env:GITHUB_ENV
  "SALTS_VERSION=$version" >> $env:GITHUB_ENV
  "QIGAO_GITHUB_PACKAGES_ROOT=$packages" >> $env:GITHUB_ENV
}
Write-Host "curl GitHub Packages verified Salts.Native $version for ${SaltsRid}: $saltsRoot"
