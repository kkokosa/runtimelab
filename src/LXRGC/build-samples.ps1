<#
.SYNOPSIS
    Publishes the benchmark sample apps self-contained onto the CUSTOM
    pluggable-write-barrier CoreCLR runtime, so every GC mode
    (workstation / server / lxrgc) runs on the exact same runtime that LXRGC
    needs for its write-barrier callback + object-scan facility. This is what
    gives the benchmark suite full barrier fidelity (RC logging + backup trace
    + Immix sweep all fire under load), instead of running the managed apps on
    a stock installed runtime where the pluggable barrier does not exist.

    For each sample it:
      1. dotnet publish -c Release -r win-x64 --self-contained (net11 SDK from
         the runtime repo), producing publish\App.exe (an apphost that probes
         its own directory for coreclr) + all managed dependencies (incl.
         ASP.NET Core for WebApi).
      2. Overlays the custom testhost's Microsoft.NETCore.App\11.0.0 binaries
         (coreclr.dll / System.Private.CoreLib.dll / clrjit.dll + the whole
         shared framework) over the publish dir, replacing the stock
         self-contained runtime with the pluggable-barrier build.
      3. Drops LXRGC.dll next to the app so DOTNET_GCName=LXRGC.dll resolves.
#>
param(
    [string]$RuntimeRepo = "C:\github\runtime",
    [string[]]$Samples = @("ConsoleApp", "WebApi", "ZeroAllocApp", "GrowingCacheApp", "GCPerfSim"),
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$dotnet = Join-Path $RuntimeRepo ".dotnet\dotnet.exe"
if (-not (Test-Path $dotnet)) { throw "net11 SDK not found at $dotnet" }

$fw = Join-Path $RuntimeRepo "artifacts\bin\testhost\net11.0-windows-$Configuration-x64\shared\Microsoft.NETCore.App\11.0.0"
if (-not (Test-Path $fw)) { throw "Custom testhost framework not found at $fw. Build the runtime first." }

$gcDll = Join-Path $root "native\obj\$Configuration\LXRGC.dll"
if (-not (Test-Path $gcDll)) { throw "LXRGC.dll not built. Run native\build.ps1 first." }

foreach ($sample in $Samples) {
    $projDir = Join-Path $root "samples\$sample"
    $pub = Join-Path $projDir "publish"
    Write-Host "`n=== Publishing $sample ===" -ForegroundColor Cyan
    if (Test-Path $pub) { Remove-Item $pub -Recurse -Force }
    & $dotnet publish (Join-Path $projDir "$sample.csproj") -c $Configuration -r win-x64 --self-contained true -o $pub `
        | Select-Object -Last 2
    if ($LASTEXITCODE -ne 0) { throw "publish failed for $sample" }

    # Overlay the pluggable-barrier runtime over the stock self-contained one.
    Copy-Item "$fw\*.dll" $pub -Force
    Copy-Item $gcDll $pub -Force
    Write-Host "overlaid custom runtime + LXRGC.dll into $sample\publish" -ForegroundColor Green
}

Write-Host "`nAll samples published onto the custom runtime." -ForegroundColor Green
