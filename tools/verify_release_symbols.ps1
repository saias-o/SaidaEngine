[CmdletBinding()]
param(
    [string]$BundleDir = '.',
    [string]$ExpectedCommit = '',
    [string]$Objdump = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$bundle = [System.IO.Path]::GetFullPath($BundleDir).TrimEnd('\', '/')
$manifestPath = Join-Path $bundle 'windows-symbols-manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "Missing symbol manifest: $manifestPath"
}
$manifest = Get-Content -Raw -Encoding UTF8 -LiteralPath $manifestPath |
    ConvertFrom-Json
if ($manifest.schema -ne 1) { throw "Unsupported symbol manifest schema" }
if ($ExpectedCommit -and $manifest.engineCommit -ne $ExpectedCommit) {
    throw "Symbol commit mismatch: expected $ExpectedCommit, got $($manifest.engineCommit)"
}

function Resolve-BundlePath([string]$Relative) {
    if ([System.IO.Path]::IsPathRooted($Relative) -or $Relative.Contains('..')) {
        throw "Unsafe manifest path: $Relative"
    }
    $full = [System.IO.Path]::GetFullPath((Join-Path $bundle $Relative))
    if (-not $full.StartsWith($bundle + [System.IO.Path]::DirectorySeparatorChar,
                              [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Manifest path escapes bundle: $Relative"
    }
    $full
}

function Check-Record($Record, [string]$Label) {
    $path = Resolve-BundlePath ([string]$Record.path)
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing $Label file: $($Record.path)"
    }
    $file = Get-Item -LiteralPath $path
    if ($file.Length -ne [long]$Record.bytes) {
        throw "$Label size mismatch: $($Record.path)"
    }
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
    if ($actual -ne [string]$Record.sha256) {
        throw "$Label SHA-256 mismatch: $($Record.path)"
    }
    $path
}

$declared = New-Object System.Collections.Generic.List[string]
$dependencyReport = Check-Record $manifest.dependencyReport "dependency report"
$declared.Add(([string]$manifest.dependencyReport.path).Replace('\', '/'))
foreach ($entry in @($manifest.binaries)) {
    $binaryPath = Check-Record $entry.binary "binary"
    $symbolPath = Check-Record $entry.symbols "symbols"
    $declared.Add(([string]$entry.binary.path).Replace('\', '/'))
    $declared.Add(([string]$entry.symbols.path).Replace('\', '/'))
    if ([System.IO.Path]::GetFileName($symbolPath) -ne [string]$entry.debugLink) {
        throw "Debug-link name mismatch for $($entry.name)"
    }

    $objdumpCommand = $null
    if ($Objdump) {
        $objdumpCommand = $Objdump
    } else {
        $found = Get-Command objdump.exe -ErrorAction SilentlyContinue
        if (-not $found) { $found = Get-Command objdump -ErrorAction SilentlyContinue }
        if ($found) { $objdumpCommand = $found.Source }
    }
    if ($objdumpCommand) {
        $sections = (& $objdumpCommand -h $binaryPath 2>&1) -join "`n"
        if ($LASTEXITCODE -ne 0 -or $sections -notmatch '\.gnu_debuglink') {
            throw "Missing .gnu_debuglink in $($entry.binary.path)"
        }
        $debugSections = (& $objdumpCommand -h $symbolPath 2>&1) -join "`n"
        if ($LASTEXITCODE -ne 0 -or $debugSections -notmatch '\.debug_info') {
            throw "Missing .debug_info in $($entry.symbols.path)"
        }
    }
}

$expected = @($declared.ToArray()) +
    @('verify_release_symbols.ps1', 'windows-symbols-manifest.json')
$actual = @(Get-ChildItem -LiteralPath $bundle -Recurse -File |
    ForEach-Object {
        $_.FullName.Substring($bundle.Length + 1).Replace('\', '/')
    } | Sort-Object)
$extra = @($actual | Where-Object { $_ -notin $expected })
$missing = @($expected | Where-Object { $_ -notin $actual })
if ($extra.Count -or $missing.Count) {
    throw "Symbol bundle inventory mismatch; extra=[$($extra -join ', ')], missing=[$($missing -join ', ')]"
}

Write-Host "WINDOWS SYMBOL BUNDLE VERIFY PASS"
Write-Host "  commit: $($manifest.engineCommit)"
Write-Host "  binaries: $(@($manifest.binaries).Count)"
