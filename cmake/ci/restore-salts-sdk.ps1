param(
  [string]$SaltsRid = ""
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

$saltsVersion = if ($env:SALTS_SDK_VERSION) { $env:SALTS_SDK_VERSION } else { "1.8.2" }
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

@"
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup><TargetFramework>net8.0</TargetFramework></PropertyGroup>
  <ItemGroup>
    <PackageReference Include="Salts.Native" Version="[$saltsVersion]" />
  </ItemGroup>
</Project>
"@ | Set-Content -LiteralPath $project

dotnet restore $project --packages $packages --configfile $config --no-cache
if ($LASTEXITCODE -ne 0) { throw "failed to restore Salts.Native $saltsVersion" }

$packageRoot = Join-Path (Join-Path $packages "salts.native") $saltsVersion
$saltsRoot = Join-Path (Join-Path $packageRoot "sdk") $SaltsRid
$configPath = Join-Path $saltsRoot "lib/cmake/Salts/SaltsConfig.cmake"
$targetsPath = Join-Path $saltsRoot "lib/cmake/Salts/SaltsTargets.cmake"
$functionHeader = Join-Path $saltsRoot "include/cmeta/function.h"

foreach ($path in @($configPath, $targetsPath, $functionHeader)) {
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
    throw "missing restored Salts 1.8.2 SDK file: $path"
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
