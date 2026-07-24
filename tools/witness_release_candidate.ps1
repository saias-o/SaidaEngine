[CmdletBinding()]
param(
    [string]$OutputDir = 'build/release/witness-v1',
    [string]$Makensis = '',
    [switch]$AllowDirty,
    [switch]$SkipBuild,
    [switch]$SkipLocalWindowsVerification
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $root 'build')).TrimEnd('\', '/')
. (Join-Path $PSScriptRoot 'git_worktree_state.ps1')
$out = if ([System.IO.Path]::IsPathRooted($OutputDir)) {
    [System.IO.Path]::GetFullPath($OutputDir)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $root $OutputDir))
}
if (-not $out.StartsWith($buildRoot + [System.IO.Path]::DirectorySeparatorChar,
                         [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "OutputDir must stay under $buildRoot"
}

Push-Location $root
try {
    $worktree = Get-GitWorktreeState
    $status = $worktree.Status
    $dirty = $worktree.Dirty
    if ($dirty -and -not $AllowDirty) {
        throw "Release candidate requires a clean Git worktree: $status (use -AllowDirty only for development proofs)"
    }
    $commit = (& git rev-parse HEAD).Trim()
    $commitTime = (& git show -s --format=%cI HEAD).Trim()

    # Packaging also needs MinGW tools such as objcopy, even when compilation
    # is skipped because the native and Web builds were already verified.
    New-Item -ItemType Directory -Force -Path build/tmp | Out-Null
    $env:PATH = 'C:\Python313;C:\msys64\usr\bin;C:\msys64\ucrt64\bin;' + $env:PATH
    $env:TMPDIR = (Resolve-Path build/tmp).Path
    $env:TMP = $env:TMPDIR
    $env:TEMP = $env:TMPDIR

    if (-not $SkipBuild) {
        & cmake --build build --parallel 4
        if ($LASTEXITCODE -ne 0) { throw "Native build failed" }
        & ctest --test-dir build --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw "Native tests failed" }
        & 'C:\msys64\usr\bin\bash.exe' web/build_web_player.sh Release
        if ($LASTEXITCODE -ne 0) { throw "Web player build failed" }
    }

    if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
    New-Item -ItemType Directory -Path $out | Out-Null
    $stage = Join-Path $out '.stage'
    $windowsDir = Join-Path $stage 'windows'
    $webExportDir = Join-Path $stage 'web-export'
    $complianceDir = Join-Path $stage 'compliance'
    $symbolsDir = Join-Path $out 'windows-symbols'
    New-Item -ItemType Directory -Path $windowsDir, $webExportDir | Out-Null

    & build/bin/SaidaEngine.exe --project WitnessGame/WitnessGame.saidaproj `
        --build $windowsDir
    if ($LASTEXITCODE -ne 0) { throw "Editor Windows Build failed" }
    & build/bin/SaidaEngine.exe --project WitnessGame/WitnessGame.saidaproj `
        --build $webExportDir --build-platform web
    if ($LASTEXITCODE -ne 0) { throw "Editor Web Build failed" }
    $webDir = Join-Path $webExportDir 'web'

    if (Test-Path -LiteralPath (Join-Path $windowsDir 'saves')) {
        throw "Windows package unexpectedly contains saves"
    }
    if (Test-Path -LiteralPath (Join-Path $webDir 'project/saves')) {
        throw "Web package unexpectedly contains saves"
    }

    $complianceArgs = @{ OutputDir = $complianceDir }
    if ($dirty) { $complianceArgs['AllowDirty'] = $true }
    & tools/generate_release_compliance.ps1 @complianceArgs
    if ($LASTEXITCODE -ne 0) { throw "Release compliance generation failed" }
    foreach ($file in Get-ChildItem -LiteralPath $complianceDir -File) {
        Copy-Item -LiteralPath $file.FullName -Destination $windowsDir
        Copy-Item -LiteralPath $file.FullName -Destination $webDir
    }
    & tools/validate_windows_dependencies.ps1 `
        -BundleDir $windowsDir `
        -EntryPoints @('Witness Game.exe') `
        -OutputPath (Join-Path $windowsDir 'windows-dependencies.json')
    if ($LASTEXITCODE -ne 0) { throw "Windows package dependency validation failed" }
    $symbolArgs = @{ OutputDir = $symbolsDir }
    if ($dirty) { $symbolArgs['AllowDirty'] = $true }
    & tools/package_release_symbols.ps1 @symbolArgs
    if ($LASTEXITCODE -ne 0) { throw "Release symbol packaging failed" }

    $installerPath = Join-Path $out 'WitnessGame-Setup.exe'
    $installerManifestPath = Join-Path $out 'WitnessGame-Setup.manifest.json'
    $installerArgs = @{
        SourceDir = $windowsDir
        OutputPath = $installerPath
        ManifestPath = $installerManifestPath
        Version = '1.0.0'
        SkipVerify = $true
    }
    if ($Makensis) { $installerArgs['Makensis'] = $Makensis }
    if ($dirty) { $installerArgs['AllowDirty'] = $true }
    & tools/build_witness_installer.ps1 @installerArgs
    if ($LASTEXITCODE -ne 0) { throw "Witness installer build failed" }
    $installerManifest =
        Get-Content -Raw -LiteralPath $installerManifestPath | ConvertFrom-Json

    $windowsArchive = Join-Path $out 'WitnessGame-Windows.zip'
    $webArchive = Join-Path $out 'WitnessGame-Web.zip'
    & tools/new_deterministic_zip.ps1 `
        -SourceDir $windowsDir -DestinationPath $windowsArchive -TimestampUtc $commitTime
    if ($LASTEXITCODE -ne 0) { throw "Deterministic Windows archive creation failed" }
    & tools/verify_deterministic_zip.ps1 `
        -SourceDir $windowsDir -ArchivePath $windowsArchive -TimestampUtc $commitTime
    if ($LASTEXITCODE -ne 0) { throw "Deterministic Windows archive verification failed" }
    & tools/new_deterministic_zip.ps1 `
        -SourceDir $webDir -DestinationPath $webArchive -TimestampUtc $commitTime
    if ($LASTEXITCODE -ne 0) { throw "Deterministic Web archive creation failed" }
    & tools/verify_deterministic_zip.ps1 `
        -SourceDir $webDir -ArchivePath $webArchive -TimestampUtc $commitTime
    if ($LASTEXITCODE -ne 0) { throw "Deterministic Web archive verification failed" }

    function File-Inventory([string]$Directory) {
        $prefix = ([System.IO.Path]::GetFullPath($Directory).TrimEnd('\', '/') +
                   [System.IO.Path]::DirectorySeparatorChar)
        @((Get-ChildItem -LiteralPath $Directory -Recurse -File | Sort-Object FullName | ForEach-Object {
            [ordered]@{
                path = $_.FullName.Substring($prefix.Length).Replace('\', '/')
                bytes = $_.Length
                sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLowerInvariant()
            }
        }))
    }
    function Archive-Record([string]$Archive, [string]$EntryPoint, [string]$SourceDir) {
        $file = Get-Item -LiteralPath $Archive
        [ordered]@{
            archive = $file.Name
            entryPoint = $EntryPoint
            bytes = $file.Length
            sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash.ToLowerInvariant()
            files = File-Inventory $SourceDir
        }
    }
    function File-Record([string]$Path) {
        $file = Get-Item -LiteralPath $Path
        [ordered]@{
            path = $file.Name
            bytes = $file.Length
            sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash.ToLowerInvariant()
        }
    }

    $manifest = [ordered]@{
        schema = 1
        engineCommit = $commit
        dirty = $dirty
        version = '1.0.0'
        generatedAtUtc = ([DateTimeOffset]::Parse($commitTime).UtcDateTime.ToString('o'))
        project = 'WitnessGame/WitnessGame.saidaproj'
        artifacts = [ordered]@{
            windows = Archive-Record $windowsArchive 'Witness Game.exe' $windowsDir
            web = Archive-Record $webArchive 'index.html' $webDir
            windowsInstaller = [ordered]@{
                installer = File-Record $installerPath
                manifest = File-Record $installerManifestPath
                entryPoint = [string]$installerManifest.entryPoint
                payloadFiles = @($installerManifest.payload).Count
                authenticode = $installerManifest.authenticode
            }
            windowsSymbols = [ordered]@{
                entryPoint = 'windows-symbols-manifest.json'
                files = File-Inventory $symbolsDir
            }
        }
    }
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 `
        -LiteralPath (Join-Path $out 'release-manifest.json')

    Copy-Item -LiteralPath tools/verify_witness_windows.ps1 -Destination $out
    Copy-Item -LiteralPath tools/verify_witness_web.ps1 -Destination $out
    @"
SaidaEngine WitnessGame release candidate

Windows clean-machine proof (PowerShell only; GPU driver/Vulkan runtime required):
  powershell -ExecutionPolicy Bypass -File .\verify_witness_windows.ps1

Installer proof (silent install, exact payload, run + restart, uninstall):
  powershell -ExecutionPolicy Bypass -File .\verify_witness_installer.ps1 -RunWitness

Web browser proofs (Python 3 + recent browser required):
  powershell -ExecutionPolicy Bypass -File .\verify_witness_web.ps1 -Browser Chrome
  powershell -ExecutionPolicy Bypass -File .\verify_witness_web.ps1 -Browser Edge -Port 18081

The scripts verify SHA-256 before execution. The Windows verifier runs the
exact extracted archive twice. The installer verifier checks every installed
file, runs Witness twice and requires a clean uninstall. The Web verifier
checks COOP/COEP and WASM MIME headers, then requires gameplay/UI PASS and
save/UI RESTART PASS.
"@ | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $out 'README.txt')

    Remove-Item -LiteralPath $stage -Recurse -Force

    if (-not $SkipLocalWindowsVerification) {
        & (Join-Path $out 'verify_witness_windows.ps1') -BundleDir $out
        if ($LASTEXITCODE -ne 0) { throw "Local Windows archive verification failed" }
        & (Join-Path $out 'verify_witness_installer.ps1') `
            -ManifestPath $installerManifestPath -RunWitness
        if ($LASTEXITCODE -ne 0) { throw "Local Windows installer verification failed" }
    }

    Write-Host "WITNESS RELEASE CANDIDATE READY: $out"
    Write-Host "  commit: $($manifest.engineCommit)"
    Write-Host "  dirty: $dirty"
    Write-Host "  Windows: $($manifest.artifacts.windows.sha256)"
    Write-Host "  Installer: $($manifest.artifacts.windowsInstaller.installer.sha256)"
    Write-Host "  Web:     $($manifest.artifacts.web.sha256)"
} finally {
    Pop-Location
}
