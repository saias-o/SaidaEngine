[CmdletBinding()]
param(
    [string]$OutputDir = 'build/release/editor-v1',
    [string]$Version = '',
    [string]$Makensis = '',
    [string]$VulkanLoader = '',
    [switch]$AllowDirty,
    [switch]$SkipBuild,
    [switch]$SkipLocalVerification
)

# Single Windows editor publication recipe. The ZIP and installer are made
# from one exact stage, then each publishable format is independently executed.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $root 'build')).TrimEnd('\', '/')
. (Join-Path $PSScriptRoot 'git_worktree_state.ps1')

function Resolve-RepoPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    [System.IO.Path]::GetFullPath((Join-Path $root $Path))
}

function Assert-UnderBuild([string]$Path, [string]$Label) {
    if (-not $Path.StartsWith(
            $buildRoot + [System.IO.Path]::DirectorySeparatorChar,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label must stay under $buildRoot"
    }
}

function Write-Utf8NoBom([string]$Path, [string]$Text) {
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, ($Text.TrimEnd() + "`r`n"), $encoding)
}

function File-Record([string]$Path, [string]$Relative = '') {
    $file = Get-Item -LiteralPath $Path
    if ([string]::IsNullOrWhiteSpace($Relative)) { $Relative = $file.Name }
    [ordered]@{
        path = $Relative.Replace('\', '/')
        bytes = $file.Length
        sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash.ToLowerInvariant()
    }
}

function File-Inventory([string]$Directory) {
    $prefix = [System.IO.Path]::GetFullPath($Directory).TrimEnd('\', '/') +
        [System.IO.Path]::DirectorySeparatorChar
    @((Get-ChildItem -LiteralPath $Directory -Recurse -File | Sort-Object FullName |
        ForEach-Object {
            File-Record $_.FullName ($_.FullName.Substring($prefix.Length))
        }))
}

$out = Resolve-RepoPath $OutputDir
Assert-UnderBuild $out 'OutputDir'

Push-Location $root
try {
    $versionHeader = Get-Content -Raw -Encoding UTF8 -LiteralPath 'src/core/EngineVersion.hpp'
    $versionMatch = [regex]::Match(
        $versionHeader, 'kProductVersion\s*=\s*"([^"]+)"')
    if (-not $versionMatch.Success) {
        throw 'Could not read kProductVersion from src/core/EngineVersion.hpp'
    }
    $sourceVersion = $versionMatch.Groups[1].Value
    if ([string]::IsNullOrWhiteSpace($Version)) { $Version = $sourceVersion }
    if ($Version -ne $sourceVersion) {
        throw "Requested version $Version does not match kProductVersion $sourceVersion"
    }
    if ($Version -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$') {
        throw "Product version is not SemVer: $Version"
    }

    $worktree = Get-GitWorktreeState
    $dirty = $worktree.Dirty
    if ($dirty -and -not $AllowDirty) {
        throw "Editor release requires a clean Git worktree: $($worktree.Status) (use -AllowDirty only for development proofs)"
    }
    $commit = (& git rev-parse HEAD).Trim()
    $commitTime = (& git show -s --format=%cI HEAD).Trim()

    New-Item -ItemType Directory -Force -Path build/tmp | Out-Null
    $env:PATH = 'C:\Python313;C:\msys64\usr\bin;C:\msys64\ucrt64\bin;' + $env:PATH
    $env:TMPDIR = (Resolve-Path build/tmp).Path
    $env:TMP = $env:TMPDIR
    $env:TEMP = $env:TMPDIR

    if (-not $SkipBuild) {
        & cmake --build build --parallel 4
        if ($LASTEXITCODE -ne 0) { throw 'Native editor build failed' }
        & ctest --test-dir build --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw 'Native editor tests failed' }
    }

    $builtVersion = (Get-Item -LiteralPath build/bin/SaidaEngine.exe).VersionInfo.ProductVersion
    if ($builtVersion -ne $Version) {
        throw "Built product version '$builtVersion' does not match '$Version'; rebuild before packaging"
    }

    if (Test-Path -LiteralPath $out) {
        Remove-Item -LiteralPath $out -Recurse -Force
    }
    New-Item -ItemType Directory -Path $out | Out-Null
    $stage = Join-Path $out 'stage'
    $symbols = Join-Path $out 'windows-symbols'
    New-Item -ItemType Directory -Path $stage | Out-Null

    $symbolArgs = @{ OutputDir = $symbols }
    if ($dirty) { $symbolArgs.AllowDirty = $true }
    & tools/package_release_symbols.ps1 @symbolArgs
    foreach ($name in @(
        'SaidaEngine.exe', 'SaidaEngineHub.exe', 'SaidaEngineRuntime.exe',
        'saida_tool.exe')) {
        Copy-Item -LiteralPath (Join-Path $symbols "binaries\$name") `
            -Destination (Join-Path $stage $name)
        $versionInfo = (Get-Item -LiteralPath (Join-Path $stage $name)).VersionInfo
        if ($versionInfo.ProductName -ne 'SaidaEngine' -or
            $versionInfo.ProductVersion -ne $Version -or
            $versionInfo.FileVersion -ne $Version) {
            throw "Windows VERSIONINFO mismatch in $name"
        }
    }
    Copy-Item -LiteralPath build/bin/glfw3.dll -Destination $stage

    if ([string]::IsNullOrWhiteSpace($VulkanLoader)) {
        $VulkanLoader = 'C:\msys64\ucrt64\bin\vulkan-1.dll'
    }
    $vulkanLoaderFull = Resolve-RepoPath $VulkanLoader
    if (-not (Test-Path -LiteralPath $vulkanLoaderFull -PathType Leaf)) {
        throw "Vulkan loader not found: $vulkanLoaderFull"
    }
    Copy-Item -LiteralPath $vulkanLoaderFull -Destination (Join-Path $stage 'vulkan-1.dll')

    Copy-Item -LiteralPath assets -Destination (Join-Path $stage 'assets') -Recurse
    New-Item -ItemType Directory -Force -Path (Join-Path $stage 'assets/fonts') | Out-Null
    foreach ($font in @(
        'LatoLatin-Regular.ttf', 'LatoLatin-Bold.ttf',
        'RobotoMono-Regular.ttf', 'NotoEmoji-Regular.ttf')) {
        $fontPath = Join-Path 'third_party/rmlui/Samples/assets' $font
        if (-not (Test-Path -LiteralPath $fontPath -PathType Leaf)) {
            throw "Required editor font not found: $fontPath"
        }
        Copy-Item -LiteralPath $fontPath -Destination (Join-Path $stage 'assets/fonts')
    }
    Copy-Item -LiteralPath build/shaders -Destination (Join-Path $stage 'shaders') -Recurse
    New-Item -ItemType Directory -Path (Join-Path $stage 'samples') | Out-Null
    Copy-Item -LiteralPath WitnessGame `
        -Destination (Join-Path $stage 'samples/WitnessGame') -Recurse
    Remove-Item -LiteralPath (Join-Path $stage 'samples/WitnessGame/saves') `
        -Recurse -Force -ErrorAction SilentlyContinue
    Copy-Item -LiteralPath LICENSE, CODE_SIGNING_POLICY.md, RELEASE.md -Destination $stage

    $complianceArgs = @{ OutputDir = (Join-Path $stage 'compliance') }
    if ($dirty) { $complianceArgs.AllowDirty = $true }
    & tools/generate_release_compliance.ps1 @complianceArgs

    $installMarker = [ordered]@{
        schema = 1
        product = 'SaidaEngine'
        version = $Version
        engineCommit = $commit
        platform = 'windows-x86_64'
        layout = [ordered]@{
            resources = '.'
            shaders = 'shaders'
            state = '%APPDATA%/SaidaEngine'
            projects = '%USERPROFILE%/Documents/SaidaEngine/Projects'
        }
    }
    Write-Utf8NoBom (Join-Path $stage 'saida-install.json') `
        ($installMarker | ConvertTo-Json -Depth 8)

    $packageReadme = @"
SaidaEngine $Version - Windows 11 x64

Quick start
1. Extract the whole ZIP to a writable folder, then run SaidaEngineHub.exe; or
2. Run the per-user SaidaEngine Setup executable.

Nothing else from the development toolchain is required: no Visual C++
Redistributable, Vulkan SDK, MSYS2, CMake or compiler. The Vulkan loader and all
non-system DLLs are included. Windows 11 and a graphics driver exposing Vulkan
1.3 are required. Install or update the normal Intel, AMD or NVIDIA graphics
driver if Vulkan 1.3 is unavailable.

This beta may be unsigned. Windows SmartScreen can therefore warn before first
launch; verify SHA256SUMS.txt and the GitHub release source before continuing.

Projects default to Documents\SaidaEngine\Projects. Application state is kept
under the current user's AppData profile, never beside the installed binaries.
"@
    Write-Utf8NoBom (Join-Path $stage 'README-WINDOWS.txt') $packageReadme

    & tools/validate_windows_dependencies.ps1 `
        -BundleDir $stage `
        -OutputPath (Join-Path $stage 'windows-dependencies.json')

    $baseName = "SaidaEngine-v$Version-windows-x64"
    $installerPath = Join-Path $out "$baseName-Setup.exe"
    $installerManifest = Join-Path $out "$baseName-Setup.manifest.json"
    $installerArgs = @{
        SourceDir = $stage
        OutputPath = $installerPath
        ManifestPath = $installerManifest
        Version = $Version
        SkipVerify = $true
    }
    if ($Makensis) { $installerArgs.Makensis = $Makensis }
    if ($dirty) { $installerArgs.AllowDirty = $true }
    & tools/build_editor_installer.ps1 @installerArgs

    $archive = Join-Path $out "$baseName.zip"
    & tools/new_deterministic_zip.ps1 `
        -SourceDir $stage -DestinationPath $archive -TimestampUtc $commitTime
    & tools/verify_deterministic_zip.ps1 `
        -SourceDir $stage -ArchivePath $archive -TimestampUtc $commitTime

    $archiveFile = Get-Item -LiteralPath $archive
    $installerFile = Get-Item -LiteralPath $installerPath
    $releaseManifest = [ordered]@{
        schema = 1
        product = 'SaidaEngine'
        version = $Version
        engineCommit = $commit
        dirty = $dirty
        generatedAtUtc = ([DateTimeOffset]::Parse($commitTime).UtcDateTime.ToString('o'))
        requirements = [ordered]@{
            os = 'Windows 11 x64'
            gpu = 'Graphics driver exposing Vulkan 1.3'
            additionalRuntimeInstall = $false
        }
        artifacts = [ordered]@{
            windowsZip = [ordered]@{
                archive = $archiveFile.Name
                bytes = $archiveFile.Length
                sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
                entryPoint = 'SaidaEngineHub.exe'
                files = File-Inventory $stage
            }
            windowsInstaller = [ordered]@{
                installer = File-Record $installerPath
                manifest = File-Record $installerManifest
                entryPoint = 'SaidaEngineHub.exe'
                authenticode = (Get-Content -Raw -LiteralPath $installerManifest |
                    ConvertFrom-Json).authenticode
            }
            windowsSymbols = [ordered]@{
                entryPoint = 'windows-symbols-manifest.json'
                files = File-Inventory $symbols
            }
        }
    }
    $manifestPath = Join-Path $out 'release-manifest.json'
    Write-Utf8NoBom $manifestPath ($releaseManifest | ConvertTo-Json -Depth 12)

    foreach ($script in @(
        'verify_editor_zip.ps1', 'verify_editor_layout.ps1',
        'verify_editor_installer.ps1', 'verify_witness_installer.ps1')) {
        Copy-Item -LiteralPath (Join-Path tools $script) -Destination $out
    }
    $hashLines = @(
        "$(($releaseManifest.artifacts.windowsZip.sha256))  $($archiveFile.Name)",
        "$((Get-FileHash -Algorithm SHA256 -LiteralPath $installerPath).Hash.ToLowerInvariant())  $($installerFile.Name)",
        "$((Get-FileHash -Algorithm SHA256 -LiteralPath $installerManifest).Hash.ToLowerInvariant())  $([System.IO.Path]::GetFileName($installerManifest))",
        "$((Get-FileHash -Algorithm SHA256 -LiteralPath $manifestPath).Hash.ToLowerInvariant())  release-manifest.json"
    )
    Write-Utf8NoBom (Join-Path $out 'SHA256SUMS.txt') ($hashLines -join "`r`n")

    if (-not $SkipLocalVerification) {
        & (Join-Path $out 'verify_editor_zip.ps1') `
            -ManifestPath $manifestPath -TimeoutSeconds 180
        & (Join-Path $out 'verify_editor_installer.ps1') `
            -ManifestPath $installerManifest -TimeoutSeconds 180
    }

    Remove-Item -LiteralPath $stage -Recurse -Force
    Write-Host "SAIDAENGINE EDITOR RELEASE CANDIDATE READY: $out"
    Write-Host "  version: $Version"
    Write-Host "  commit: $commit"
    Write-Host "  dirty: $dirty"
    Write-Host "  ZIP: $($releaseManifest.artifacts.windowsZip.sha256)"
    Write-Host "  installer: $((Get-FileHash -Algorithm SHA256 -LiteralPath $installerPath).Hash.ToLowerInvariant())"
} finally {
    Pop-Location
}
