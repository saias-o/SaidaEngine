[CmdletBinding()]
param(
    [string]$OutputDir = 'build/release/engine',
    [string]$SaidaTool = 'build/bin/saida_tool.exe',
    [string]$DesktopRuntime = 'build/bin/SaidaEngineRuntime.exe',
    [string]$WebPlayerDir = 'build-web-player',
    [string]$AuthoringWasmDir = 'build-authoring-wasm',
    [string]$AuthoringRuntimeDir = 'build-web',
    [string]$FixturesDir = 'tests/fixtures/v1-format',
    [switch]$AllowDirty,
    [switch]$SkipVerify
)

# Produces build/release/engine/release-manifest.json: the immutable identity of
# one engine bundle. It pins the format/contract versions (read from the shipped
# saida_tool, the single source of truth) plus the SHA-256 of saida_tool, the
# desktop runtime, the Web player, the authoring WASM surfaces and every frozen
# V1 format fixture. The Saida platform pins this manifest so its Docker
# tool, its served Web bundle and its fixtures cannot silently diverge.

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

function Resolve-Local([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) { return $Path }
    return [System.IO.Path]::GetFullPath((Join-Path $root $Path))
}

# Deliverable web bundle files only; never CMake/ninja intermediates.
$bundleExtensions = @('.html', '.js', '.mjs', '.wasm', '.data')

function File-Record([string]$FullPath, [string]$RelPath) {
    $item = Get-Item -LiteralPath $FullPath
    [ordered]@{
        path = $RelPath.Replace('\', '/')
        bytes = $item.Length
        sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $FullPath).Hash.ToLowerInvariant()
    }
}

function Bundle-Inventory([string]$Directory, [string]$EntryPoint) {
    $dir = Resolve-Local $Directory
    if (-not (Test-Path -LiteralPath $dir)) { throw "missing bundle directory: $dir" }
    $prefix = ([System.IO.Path]::GetFullPath($dir).TrimEnd('\', '/') +
               [System.IO.Path]::DirectorySeparatorChar)
    $files = @(Get-ChildItem -LiteralPath $dir -Recurse -File |
        Where-Object { $bundleExtensions -contains $_.Extension.ToLowerInvariant() } |
        Sort-Object FullName |
        ForEach-Object { File-Record $_.FullName $_.FullName.Substring($prefix.Length) })
    if ($files.Count -eq 0) { throw "no deliverable files under: $dir" }
    if (-not ($files | Where-Object { $_.path -eq $EntryPoint })) {
        throw "bundle $Directory is missing its entry point $EntryPoint"
    }
    [ordered]@{ entryPoint = $EntryPoint; files = $files }
}

function Exact-Directory-Inventory([string]$Directory, [string]$EntryPoint) {
    $dir = Resolve-Local $Directory
    if (-not (Test-Path -LiteralPath $dir)) { throw "missing bundle directory: $dir" }
    $prefix = ([System.IO.Path]::GetFullPath($dir).TrimEnd('\', '/') +
               [System.IO.Path]::DirectorySeparatorChar)
    $files = @(Get-ChildItem -LiteralPath $dir -Recurse -File | Sort-Object FullName |
        ForEach-Object { File-Record $_.FullName $_.FullName.Substring($prefix.Length) })
    if ($files.Count -eq 0) { throw "no deliverable files under: $dir" }
    if (-not ($files | Where-Object { $_.path -eq $EntryPoint })) {
        throw "bundle $Directory is missing its entry point $EntryPoint"
    }
    [ordered]@{ entryPoint = $EntryPoint; exact = $true; files = $files }
}

function Fixture-Inventory([string]$Directory) {
    $dir = Resolve-Local $Directory
    if (-not (Test-Path -LiteralPath $dir)) { throw "missing fixtures directory: $dir" }
    $prefix = ([System.IO.Path]::GetFullPath($dir).TrimEnd('\', '/') +
               [System.IO.Path]::DirectorySeparatorChar)
    @(Get-ChildItem -LiteralPath $dir -Recurse -File | Sort-Object FullName |
        ForEach-Object { File-Record $_.FullName $_.FullName.Substring($prefix.Length) })
}

Push-Location $root
try {
    $worktree = Get-GitWorktreeState
    $status = $worktree.Status
    $dirty = $worktree.Dirty
    if ($dirty -and -not $AllowDirty) {
        throw "Release manifest requires a clean Git worktree: $status (use -AllowDirty only for development proofs)"
    }

    if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
    New-Item -ItemType Directory -Path $out | Out-Null
    $complianceDir = Join-Path $out 'compliance'
    $complianceArgs = @{ OutputDir = $complianceDir }
    if ($dirty) { $complianceArgs['AllowDirty'] = $true }
    & (Join-Path $PSScriptRoot 'generate_release_compliance.ps1') @complianceArgs
    if ($LASTEXITCODE -ne 0) { throw "release compliance generation failed" }
    $symbolsDir = Join-Path $out 'windows-symbols'
    $symbolArgs = @{ OutputDir = $symbolsDir }
    if ($dirty) { $symbolArgs['AllowDirty'] = $true }
    & (Join-Path $PSScriptRoot 'package_release_symbols.ps1') @symbolArgs
    if ($LASTEXITCODE -ne 0) { throw "release symbol packaging failed" }

    $toolPath = Resolve-Local $SaidaTool
    if (-not (Test-Path -LiteralPath $toolPath)) { throw "missing saida_tool: $toolPath" }

    # The shipped tool is the source of truth for the engine and format versions.
    $describe = (& $toolPath describe-engine | Out-String)
    if ($LASTEXITCODE -ne 0) { throw "saida_tool describe-engine failed" }
    $manifestJson = $describe | ConvertFrom-Json

    $artifacts = [ordered]@{}
    $artifacts['saidaTool'] = File-Record $toolPath ([System.IO.Path]::GetFileName($toolPath))
    $runtimePath = Resolve-Local $DesktopRuntime
    if (Test-Path -LiteralPath $runtimePath) {
        $artifacts['desktopRuntime'] = File-Record $runtimePath ([System.IO.Path]::GetFileName($runtimePath))
    }
    $artifacts['webPlayer'] = Bundle-Inventory $WebPlayerDir 'index.html'
    $artifacts['authoringWasm'] = Bundle-Inventory $AuthoringWasmDir 'saida_authoring.mjs'
    $artifacts['authoringRuntime'] = Bundle-Inventory $AuthoringRuntimeDir 'index.html'
    $artifacts['compliance'] = Exact-Directory-Inventory $complianceDir 'sbom.spdx.json'
    $artifacts['windowsSymbols'] =
        Exact-Directory-Inventory $symbolsDir 'windows-symbols-manifest.json'

    $commit = (& git rev-parse HEAD).Trim()
    $commitTime = (& git show -s --format=%cI HEAD).Trim()
    $manifest = [ordered]@{
        schema = 1
        engineCommit = $commit
        dirty = $dirty
        generatedAtUtc = ([DateTimeOffset]::Parse($commitTime).UtcDateTime.ToString('o'))
        engineVersion = $manifestJson.engineVersion
        formats = $manifestJson.formats
        artifacts = $artifacts
        fixtures = Fixture-Inventory $FixturesDir
    }

    $manifestPath = Join-Path $out 'release-manifest.json'
    $manifest | ConvertTo-Json -Depth 12 | Set-Content -Encoding UTF8 -LiteralPath $manifestPath

    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'verify_engine_release.ps1') -Destination $out

    if (-not $SkipVerify) {
        $verifyArgs = @{
            ManifestPath = $manifestPath
            SaidaTool = $toolPath
            WebPlayerDir = (Resolve-Local $WebPlayerDir)
            AuthoringWasmDir = (Resolve-Local $AuthoringWasmDir)
            AuthoringRuntimeDir = (Resolve-Local $AuthoringRuntimeDir)
            ComplianceDir = $complianceDir
            SymbolsDir = $symbolsDir
            FixturesDir = (Resolve-Local $FixturesDir)
        }
        if ($artifacts.Contains('desktopRuntime')) { $verifyArgs['DesktopRuntime'] = $runtimePath }
        & (Join-Path $out 'verify_engine_release.ps1') @verifyArgs
        if ($LASTEXITCODE -ne 0) { throw "release manifest self-verification failed" }
    }

    Write-Host "ENGINE RELEASE MANIFEST READY: $manifestPath"
    Write-Host "  commit: $($manifest.engineCommit)  dirty: $dirty"
    Write-Host "  engineVersion: $($manifest.engineVersion)  opVersion: $($manifestJson.formats.opVersion)"
    Write-Host "  saida_tool: $($artifacts.saidaTool.sha256)"
    Write-Host "  fixtures: $($manifest.fixtures.Count) frozen files"
} finally {
    Pop-Location
}
