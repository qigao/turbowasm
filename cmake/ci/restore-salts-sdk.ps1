param(
  [string]$SaltsRid = "",
  [switch]$WithSaltsUtils,
  [switch]$Local
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$requiredEnvironment = @("GITHUB_TOKEN")
if (-not $Local) { $requiredEnvironment += @("RUNNER_TEMP", "GITHUB_ENV") }
foreach ($name in $requiredEnvironment) {
  $value = [Environment]::GetEnvironmentVariable($name)
  if ([string]::IsNullOrWhiteSpace($value)) {
    throw "$name is required"
  }
}

if ($Local -and [string]::IsNullOrWhiteSpace($SaltsRid)) {
  throw "-SaltsRid is required for local restore"
}
if ([string]::IsNullOrWhiteSpace($SaltsRid)) {
  switch ($env:RUNNER_OS) {
    "Linux" {
      if ($env:RUNNER_ARCH -ne "X64") { throw "unsupported Linux Salts.Native architecture: $env:RUNNER_ARCH" }
      $SaltsRid = "linux-x64"
    }
    "Windows" {
      if ($env:RUNNER_ARCH -ne "X64") { throw "unsupported Windows Salts.Native architecture: $env:RUNNER_ARCH" }
      $SaltsRid = "windows-x64"
    }
    "macOS" {
      switch ($env:RUNNER_ARCH) {
        "X64" { $SaltsRid = "macos-x64" }
        "ARM64" { $SaltsRid = "macos-arm64" }
        default { throw "unsupported macOS Salts.Native architecture: $env:RUNNER_ARCH" }
      }
    }
    default { throw "unsupported runner OS for Salts.Native: $env:RUNNER_OS" }
  }
}

if ($env:SALTS_SDK_VERSION) {
  throw "SALTS_SDK_VERSION is forbidden; GitHub Packages dependencies must resolve latest"
}
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../.."))
$restoreRoot = if ($Local) { Join-Path $repositoryRoot "build/native-sdk" } else { $env:RUNNER_TEMP }
$packages = if ($env:QIGAO_NUGET_PACKAGES) { $env:QIGAO_NUGET_PACKAGES } else { Join-Path $restoreRoot "qigao-nuget" }
$packages = [IO.Path]::GetFullPath($packages)
$config = Join-Path $repositoryRoot "cmake/vcpkg-cache.nuget.config"
$project = Join-Path $restoreRoot "turbowasm-salts-sdk-restore.csproj"
New-Item -ItemType Directory -Path $restoreRoot -Force | Out-Null

$saltsUtilsReference = if ($WithSaltsUtils) {
  '    <PackageReference Include="SaltsUtils.Native" Version="*" />'
} else {
  ""
}

@"
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net8.0</TargetFramework>
    <RestorePackagesWithLockFile>false</RestorePackagesWithLockFile>
  </PropertyGroup>
  <ItemGroup>
    <PackageReference Include="Salts.Native" Version="*" />
$saltsUtilsReference
  </ItemGroup>
</Project>
"@ | Set-Content -LiteralPath $project

dotnet restore $project --packages $packages --configfile $config --no-cache --force-evaluate
if ($LASTEXITCODE -ne 0) { throw "failed to restore latest Salts.Native" }

$assetsPath = Join-Path $restoreRoot "obj/project.assets.json"
if (-not (Test-Path -LiteralPath $assetsPath -PathType Leaf)) {
  throw "missing NuGet restore assets: $assetsPath"
}
$assets = Get-Content -LiteralPath $assetsPath -Raw | ConvertFrom-Json
$saltsLibraries = @(
  $assets.libraries.PSObject.Properties.Name |
    Where-Object { $_ -like "Salts.Native/*" }
)
if ($saltsLibraries.Count -ne 1) {
  throw "expected exactly one restored Salts.Native package, found $($saltsLibraries.Count)"
}
$saltsVersion = ($saltsLibraries[0] -split "/", 2)[1]

$packageRoot = Join-Path (Join-Path $packages "salts.native") $saltsVersion
$saltsRoot = Join-Path (Join-Path $packageRoot "sdk") $SaltsRid
$configPath = Join-Path $saltsRoot "lib/cmake/Salts/SaltsConfig.cmake"
$targetsPath = Join-Path $saltsRoot "lib/cmake/Salts/SaltsTargets.cmake"
$functionHeader = Join-Path $saltsRoot "include/cmeta/function.h"

foreach ($path in @($configPath, $targetsPath, $functionHeader)) {
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
    throw "missing restored Salts SDK file: $path"
  }
}

$env:SALTS_ROOT = $saltsRoot
$env:SALTS_VERSION = $saltsVersion
$env:QIGAO_NUGET_PACKAGES = $packages
if (-not $Local) {
  "SALTS_ROOT=$saltsRoot" >> $env:GITHUB_ENV
  "SALTS_VERSION=$saltsVersion" >> $env:GITHUB_ENV
  "QIGAO_NUGET_PACKAGES=$packages" >> $env:GITHUB_ENV
}
Write-Host "Restored Salts.Native $saltsVersion ($SaltsRid): $saltsRoot"

if ($WithSaltsUtils) {
  $utilsLibraries = @(
    $assets.libraries.PSObject.Properties.Name |
      Where-Object { $_ -like "SaltsUtils.Native/*" }
  )
  if ($utilsLibraries.Count -ne 1) {
    throw "expected exactly one restored SaltsUtils.Native package, found $($utilsLibraries.Count)"
  }

  $utilsVersion = ($utilsLibraries[0] -split "/", 2)[1]
  $utilsPackageRoot = Join-Path (Join-Path $packages "saltsutils.native") $utilsVersion
  $utilsRoot = Join-Path (Join-Path $utilsPackageRoot "sdk") $SaltsRid
  $utilsConfig = Join-Path $utilsRoot "lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake"
  $utilsTargets = Join-Path $utilsRoot "lib/cmake/SaltsUtils/SaltsUtilsTargets.cmake"
  $producerHeader = Join-Path $utilsRoot "include/data_bind_cmeta_adapter_plan.h"

  foreach ($path in @($utilsConfig, $utilsTargets, $producerHeader)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
      throw "missing restored SaltsUtils SDK file: $path"
    }
  }

  $env:SALTS_UTILS_ROOT = $utilsRoot
  if (-not $Local) {
    "SALTS_UTILS_ROOT=$utilsRoot" >> $env:GITHUB_ENV
  }
  Write-Host "Restored SaltsUtils.Native $utilsVersion ($SaltsRid): $utilsRoot"
}
