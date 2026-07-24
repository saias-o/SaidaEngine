[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$SourceDir,
    [Parameter(Mandatory = $true)][string]$ArchivePath,
    [string]$TimestampUtc = ''
)

# Verify that a ZIP is an exact canonical representation of SourceDir. This
# checks paths, ordering, timestamps, sizes and every uncompressed byte without
# trusting extraction behavior.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

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
    return [DateTimeOffset]::new(
        $utc.Year, $utc.Month, $utc.Day, $utc.Hour, $utc.Minute,
        ($utc.Second - ($utc.Second % 2)), [TimeSpan]::Zero)
}

$source = Resolve-Local $SourceDir
$archivePathFull = Resolve-Local $ArchivePath
if (-not (Test-Path -LiteralPath $source -PathType Container)) {
    throw "Source directory not found: $source"
}
if (-not (Test-Path -LiteralPath $archivePathFull -PathType Leaf)) {
    throw "Archive not found: $archivePathFull"
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

$sourcePrefix = $source.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
$expected = New-Object 'System.Collections.Generic.Dictionary[string,string]' (
    [System.StringComparer]::Ordinal)
$caseGuard = New-Object 'System.Collections.Generic.HashSet[string]' (
    [System.StringComparer]::OrdinalIgnoreCase)
foreach ($item in Get-ChildItem -LiteralPath $source -Recurse -Force) {
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Source contains symlink/reparse point: $($item.FullName)"
    }
    if ($item.PSIsContainer) { continue }
    $relative = $item.FullName.Substring($sourcePrefix.Length).Replace('\', '/')
    if (-not $caseGuard.Add($relative)) {
        throw "Source contains case-colliding paths: $relative"
    }
    $expected.Add($relative, $item.FullName)
}

$failures = New-Object System.Collections.Generic.List[string]
$seen = New-Object 'System.Collections.Generic.HashSet[string]' (
    [System.StringComparer]::Ordinal)
$seenCase = New-Object 'System.Collections.Generic.HashSet[string]' (
    [System.StringComparer]::OrdinalIgnoreCase)

Add-Type -AssemblyName System.IO.Compression
$stream = [System.IO.File]::OpenRead($archivePathFull)
try {
    $zip = New-Object System.IO.Compression.ZipArchive(
        $stream, [System.IO.Compression.ZipArchiveMode]::Read, $true)
    try {
        $previous = $null
        foreach ($entry in $zip.Entries) {
            $path = [string]$entry.FullName
            if ([string]::IsNullOrWhiteSpace($path) -or
                $path.EndsWith('/') -or
                $path.StartsWith('/') -or
                $path.Contains('\') -or
                $path.Split('/') -contains '..') {
                $failures.Add("unsafe or directory entry: $path")
                continue
            }
            if ($null -ne $previous -and
                [System.StringComparer]::Ordinal.Compare($previous, $path) -ge 0) {
                $failures.Add("entries are not strictly ordinal-sorted: $previous then $path")
            }
            $previous = $path
            if (-not $seen.Add($path) -or -not $seenCase.Add($path)) {
                $failures.Add("duplicate or case-colliding entry: $path")
                continue
            }
            if (-not $expected.ContainsKey($path)) {
                $failures.Add("unexpected archive entry: $path")
                continue
            }
            # ZIP has no timezone field. Compare the canonical wall-clock
            # components, not the local offset assigned by the reader.
            if ($entry.LastWriteTime.DateTime -ne $timestamp.DateTime) {
                $failures.Add(
                    "timestamp mismatch for ${path}: expected $($timestamp.DateTime.ToString('s')), " +
                    "got $($entry.LastWriteTime.DateTime.ToString('s'))")
            }
            $sourceFile = Get-Item -LiteralPath $expected[$path]
            if ($entry.Length -ne $sourceFile.Length) {
                $failures.Add(
                    "size mismatch for ${path}: expected $($sourceFile.Length), got $($entry.Length)")
                continue
            }
            $entryStream = $entry.Open()
            try {
                $actualHash = (Get-FileHash -Algorithm SHA256 -InputStream $entryStream).Hash
            } finally {
                $entryStream.Dispose()
            }
            $expectedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $sourceFile.FullName).Hash
            if ($actualHash -ne $expectedHash) {
                $failures.Add("content mismatch for $path")
            }
        }
    } finally {
        $zip.Dispose()
    }
} finally {
    $stream.Dispose()
}

foreach ($path in $expected.Keys) {
    if (-not $seen.Contains($path)) {
        $failures.Add("missing archive entry: $path")
    }
}
if ($failures.Count -gt 0) {
    Write-Host "DETERMINISTIC ZIP VERIFY FAILED ($($failures.Count) issue(s)):"
    foreach ($failure in $failures) {
        Write-Host "  - $failure"
    }
    exit 1
}

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archivePathFull).Hash.ToLowerInvariant()
Write-Host "DETERMINISTIC ZIP VERIFY PASS"
Write-Host "  entries: $($expected.Count)"
Write-Host "  sha256: $hash"
exit 0
