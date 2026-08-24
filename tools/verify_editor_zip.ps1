[CmdletBinding()]
param(
    [string]$ManifestPath = 'build/release/editor-v1/release-manifest.json',
    [int]$TimeoutSeconds = 180,
    [switch]$KeepWork
)

# Verifies the actual publishable ZIP: archive hash, exact extracted inventory,
# then the complete editor-layout proof using only extracted files.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$manifestFull = [System.IO.Path]::GetFullPath($ManifestPath)
if (-not (Test-Path -LiteralPath $manifestFull -PathType Leaf)) {
    throw "Editor release manifest not found: $manifestFull"
}
$bundle = Split-Path -Parent $manifestFull
$manifest = Get-Content -Raw -LiteralPath $manifestFull | ConvertFrom-Json
if ($manifest.schema -ne 1 -or $manifest.product -ne 'SaidaEngine') {
    throw 'Unsupported SaidaEngine release manifest'
}
$archiveRecord = $manifest.artifacts.windowsZip
$archive = Join-Path $bundle ([string]$archiveRecord.archive)
if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) {
    throw "Editor ZIP not found: $archive"
}
$actualArchiveHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
if ($actualArchiveHash -ne [string]$archiveRecord.sha256) {
    throw "Editor ZIP SHA-256 mismatch: $actualArchiveHash"
}

$work = Join-Path $bundle '.verify-editor-zip'
if (Test-Path -LiteralPath $work) {
    Remove-Item -LiteralPath $work -Recurse -Force
}
$extracted = Join-Path $work 'extracted'
New-Item -ItemType Directory -Path $extracted | Out-Null
try {
    Expand-Archive -LiteralPath $archive -DestinationPath $extracted
    $expected = New-Object 'System.Collections.Generic.Dictionary[string,object]' (
        [System.StringComparer]::Ordinal)
    foreach ($record in @($archiveRecord.files)) {
        $expected.Add([string]$record.path, $record)
    }
    $prefix = $extracted.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    $actualPaths = New-Object System.Collections.Generic.List[string]
    foreach ($file in Get-ChildItem -LiteralPath $extracted -Recurse -File) {
        $relative = $file.FullName.Substring($prefix.Length).Replace('\', '/')
        $actualPaths.Add($relative)
        if (-not $expected.ContainsKey($relative)) {
            throw "Unexpected file in editor ZIP: $relative"
        }
        $record = $expected[$relative]
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash.ToLowerInvariant()
        if ($file.Length -ne [long]$record.bytes -or $hash -ne [string]$record.sha256) {
            throw "Editor ZIP payload mismatch: $relative"
        }
    }
    if ($actualPaths.Count -ne $expected.Count) {
        $missing = @($expected.Keys | Where-Object { $_ -notin $actualPaths })
        throw "Editor ZIP is missing files: $($missing -join ', ')"
    }

    & (Join-Path $PSScriptRoot 'verify_editor_layout.ps1') `
        -InstallDir $extracted -WorkDir (Join-Path $work 'layout-work') `
        -TimeoutSeconds $TimeoutSeconds

    Write-Host 'SAIDAENGINE EDITOR ZIP VERIFY PASS'
    Write-Host "  archive: $([System.IO.Path]::GetFileName($archive))"
    Write-Host "  files: $($expected.Count)"
} catch {
    $KeepWork = $true
    throw
} finally {
    if (-not $KeepWork -and (Test-Path -LiteralPath $work)) {
        Remove-Item -LiteralPath $work -Recurse -Force
    } elseif (Test-Path -LiteralPath $work) {
        Write-Host "Editor ZIP verification files kept in $work"
    }
}
