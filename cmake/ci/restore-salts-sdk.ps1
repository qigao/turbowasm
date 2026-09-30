param(
  [string]$SaltsRid = "",
  [switch]$WithSaltsUtils
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

foreach ($name in @("GITHUB_TOKEN", "RUNNER_TEMP", "GITHUB_ENV")) {
  $value = [Environment]::GetEnvironmentVariable($name)
  if ([string]::IsNullOrWhiteSpace($value)) {
    throw "$name is required"
  }
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
$packages = if ($env:QIGAO_NUGET_PACKAGES) { $env:QIGAO_NUGET_PACKAGES } else { Join-Path $env:RUNNER_TEMP "qigao-nuget" }
$config = Join-Path $env:RUNNER_TEMP "qigao-nuget.config"
$project = Join-Path $env:RUNNER_TEMP "turbowasm-salts-sdk-restore.csproj"

@'
<?xml version="1.0" encoding="utf-8"?>
<configuration>
  <packageSources><clear /></packageSources>
</configuration>
'@ | Set-Content -LiteralPath $config

dotnet nuget add source "https://nuget.pkg.github.com/qigao/index.json" --name github --username qigao --password $env:GITHUB_TOKEN --store-password-in-clear-text --configfile $config
if ($LASTEXITCODE -ne 0) { throw "failed to configure GitHub Packages source" }

$saltsUtilsReference = if ($WithSaltsUtils) {
  '    <PackageReference Include="SaltsUtils.Native" Version="*" />'
} else {
  ""
}

@"
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup><TargetFramework>net8.0</TargetFramework></PropertyGroup>
  <ItemGroup>
    <PackageReference Include="Salts.Native" Version="*" />
$saltsUtilsReference
  </ItemGroup>
</Project>
"@ | Set-Content -LiteralPath $project

dotnet restore $project --packages $packages --configfile $config --no-cache
if ($LASTEXITCODE -ne 0) { throw "failed to restore latest Salts.Native" }

$assetsPath = Join-Path $env:RUNNER_TEMP "obj/project.assets.json"
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

$targets = Get-Content -LiteralPath $targetsPath -Raw
foreach ($target in @("Salts::CMeta", "Salts::SIMD", "Salts::CFlow")) {
  if (-not $targets.Contains($target)) {
    throw "Salts.Native $saltsVersion does not export $target"
  }
}

"SALTS_ROOT=$saltsRoot" >> $env:GITHUB_ENV
"QIGAO_NUGET_PACKAGES=$packages" >> $env:GITHUB_ENV
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

  $utilsTargetsText = Get-Content -LiteralPath $utilsTargets -Raw
  if (-not $utilsTargetsText.Contains("Salts::DataBindProducer")) {
    throw "SaltsUtils.Native $utilsVersion does not export Salts::DataBindProducer"
  }

  "SALTS_UTILS_ROOT=$utilsRoot" >> $env:GITHUB_ENV
  Write-Host "Restored SaltsUtils.Native $utilsVersion ($SaltsRid): $utilsRoot"
}
