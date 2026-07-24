[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$SourceDir,
    [Parameter(Mandatory = $true)][string]$DestinationPath,
    [string]$TimestampUtc = ''
)

# Create a canonical ZIP from a release staging directory. Entry order,
# metadata and timestamps are fixed so identical staged bytes produce an
# identical archive. Symlinks/reparse points and case-colliding paths are
# rejected because Windows extraction would make their meaning ambiguous.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $root 'build')).TrimEnd('\', '/')

function Resolve-Local([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $root $Path))
}

function Normalize-ZipTimestamp([DateTimeOffset]$Timestamp) {
    $utc = $Timestamp.ToUniversalTime()
    if ($utc.Year -lt 1980 -or $utc.Year -gt 2107) {
        throw "ZIP timestamp must be between 1980 and 2107: $($utc.ToString('o'))"
    }
    # The ZIP format stores seconds with a two-second resolution.
    return [DateTimeOffset]::new(
        $utc.Year, $utc.Month, $utc.Day, $utc.Hour, $utc.Minute,
        ($utc.Second - ($utc.Second % 2)), [TimeSpan]::Zero)
}

$source = Resolve-Local $SourceDir
$destination = Resolve-Local $DestinationPath
if (-not (Test-Path -LiteralPath $source -PathType Container)) {
    throw "Source directory not found: $source"
}
if (-not $destination.StartsWith(
        $buildRoot + [System.IO.Path]::DirectorySeparatorChar,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "DestinationPath must stay under $buildRoot"
}
$sourcePrefix = $source.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
if ($destination.StartsWith($sourcePrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "DestinationPath must not be inside SourceDir"
}
if ([System.IO.Path]::GetExtension($destination) -ne '.zip') {
    throw "DestinationPath must use the .zip extension"
}

if ([string]::IsNullOrWhiteSpace($TimestampUtc)) {
    Push-Location $root
    try {
        $TimestampUtc = (& git show -s --format=%cI HEAD).Trim()
        if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($TimestampUtc)) {
            throw "Could not read the Git commit timestamp"
        }
    } finally {
        Pop-Location
    }
}
$timestamp = Normalize-ZipTimestamp ([DateTimeOffset]::Parse($TimestampUtc))

$entriesByPath = New-Object 'System.Collections.Generic.Dictionary[string,string]' (
    [System.StringComparer]::OrdinalIgnoreCase)
foreach ($item in Get-ChildItem -LiteralPath $source -Recurse -Force) {
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Release ZIP refuses symlink/reparse point: $($item.FullName)"
    }
    if ($item.PSIsContainer) { continue }
    $relative = $item.FullName.Substring($sourcePrefix.Length).Replace('\', '/')
    if ([string]::IsNullOrWhiteSpace($relative) -or
        $relative.StartsWith('/') -or
        $relative.Split('/') -contains '..') {
        throw "Unsafe ZIP entry path: $relative"
    }
    if ($entriesByPath.ContainsKey($relative)) {
        throw "Case-colliding ZIP entry paths: $relative"
    }
    $entriesByPath.Add($relative, $item.FullName)
}
if ($entriesByPath.Count -eq 0) {
    throw "Source directory contains no files: $source"
}

$entryPaths = [string[]]@($entriesByPath.Keys)
[Array]::Sort($entryPaths, [System.StringComparer]::Ordinal)

$parent = Split-Path -Parent $destination
New-Item -ItemType Directory -Force -Path $parent | Out-Null
if (Test-Path -LiteralPath $destination) {
    Remove-Item -LiteralPath $destination -Force
}

Add-Type -AssemblyName System.IO.Compression
$fileStream = [System.IO.File]::Open(
    $destination, [System.IO.FileMode]::CreateNew,
    [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
try {
    $zip = New-Object System.IO.Compression.ZipArchive(
        $fileStream, [System.IO.Compression.ZipArchiveMode]::Create, $true)
    try {
        foreach ($entryPath in $entryPaths) {
            $entry = $zip.CreateEntry(
                $entryPath, [System.IO.Compression.CompressionLevel]::Optimal)
            $entry.LastWriteTime = $timestamp
            $entry.ExternalAttributes = 0
            $input = [System.IO.File]::Open(
                $entriesByPath[$entryPath], [System.IO.FileMode]::Open,
                [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
            try {
                $output = $entry.Open()
                try {
                    $input.CopyTo($output)
                } finally {
                    $output.Dispose()
                }
            } finally {
                $input.Dispose()
            }
        }
    } finally {
        $zip.Dispose()
    }
} finally {
    $fileStream.Dispose()
}

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash.ToLowerInvariant()
Write-Host "DETERMINISTIC ZIP READY: $destination"
Write-Host "  entries: $($entryPaths.Count)"
Write-Host "  timestamp: $($timestamp.ToString('o'))"
Write-Host "  sha256: $hash"
