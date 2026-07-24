[CmdletBinding()]
param(
    [string]$SourceDir = 'build/witness-editor-build',
    [string]$OutputPath = 'build/release/witness-installer/WitnessGame-Setup.exe',
    [string]$ManifestPath = '',
    [string]$Makensis = '',
    [string]$Version = '1.0.0',
    [switch]$AllowDirty,
    [switch]$SkipVerify
)

# Compile a deterministic, per-user NSIS installer from an already validated
# Witness Windows stage. The signing key is deliberately not accepted here:
# Authenticode signing is the final publication operation and its resulting
# bytes must be inventoried by a separate trusted signing session.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $root 'build')).TrimEnd('\', '/')
. (Join-Path $PSScriptRoot 'git_worktree_state.ps1')

function Resolve-Local([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $root $Path))
}

function Assert-BuildPath([string]$Path, [string]$Label) {
    if (-not $Path.StartsWith(
            $buildRoot + [System.IO.Path]::DirectorySeparatorChar,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label must stay under $buildRoot"
    }
}

function Resolve-Makensis([string]$Provided) {
    if ($Provided) {
        $candidate = if ([System.IO.Path]::IsPathRooted($Provided)) {
            [System.IO.Path]::GetFullPath($Provided)
        } else {
            (Get-Command $Provided -ErrorAction Stop).Source
        }
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            throw "makensis not found: $candidate"
        }
        return $candidate
    }
    $command = Get-Command makensis.exe -ErrorAction SilentlyContinue
    if (-not $command) { $command = Get-Command makensis -ErrorAction SilentlyContinue }
    if ($command) { return $command.Source }
    $msysCandidate = 'C:\msys64\mingw64\bin\makensis.exe'
    if (Test-Path -LiteralPath $msysCandidate -PathType Leaf) {
        return $msysCandidate
    }
    throw "makensis 3.12+ is required"
}

function File-Record([string]$Path, [string]$Relative) {
    $file = Get-Item -LiteralPath $Path
    [ordered]@{
        path = $Relative.Replace('\', '/')
        bytes = $file.Length
        sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash.ToLowerInvariant()
    }
}

function Payload-Inventory([string]$Directory) {
    $prefix = $Directory.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    $caseGuard = New-Object 'System.Collections.Generic.HashSet[string]' (
        [System.StringComparer]::OrdinalIgnoreCase)
    $filesByPath = New-Object 'System.Collections.Generic.Dictionary[string,string]' (
        [System.StringComparer]::Ordinal)
    foreach ($item in Get-ChildItem -LiteralPath $Directory -Recurse -Force) {
        if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Installer payload refuses symlink/reparse point: $($item.FullName)"
        }
        if ($item.PSIsContainer) { continue }
        $relative = $item.FullName.Substring($prefix.Length).Replace('\', '/')
        if (-not $caseGuard.Add($relative)) {
            throw "Installer payload contains case-colliding paths: $relative"
        }
        $filesByPath.Add($relative, $item.FullName)
    }
    $paths = [string[]]@($filesByPath.Keys)
    [Array]::Sort($paths, [System.StringComparer]::Ordinal)
    $records = New-Object System.Collections.Generic.List[object]
    foreach ($relative in $paths) {
        $records.Add((File-Record $filesByPath[$relative] $relative))
    }
    if ($records.Count -eq 0) { throw "Installer payload is empty: $Directory" }
    $records.ToArray()
}

function Escape-NsisLiteral([string]$Value, [string]$Label) {
    if ($Value.Contains('"') -or $Value.Contains("`r") -or $Value.Contains("`n")) {
        throw "$Label cannot be represented safely in the NSIS script: $Value"
    }
    $Value.Replace('$', '$$')
}

function Write-PayloadInclude(
    [string]$Directory,
    [object[]]$Records,
    [string]$IncludePath
) {
    $lines = New-Object System.Collections.Generic.List[string]
    $currentDirectory = $null
    foreach ($record in $Records) {
        $relative = [string]$record.path
        $relativeWindows = $relative.Replace('/', '\')
        $directoryPart = [System.IO.Path]::GetDirectoryName($relativeWindows)
        if ($directoryPart -ne $currentDirectory) {
            $destination = if ([string]::IsNullOrWhiteSpace($directoryPart)) {
                '$INSTDIR'
            } else {
                '$INSTDIR\' + (Escape-NsisLiteral $directoryPart 'payload directory')
            }
            $lines.Add("SetOutPath `"$destination`"")
            $currentDirectory = $directoryPart
        }
        $fileName = Escape-NsisLiteral `
            ([System.IO.Path]::GetFileName($relativeWindows)) 'payload filename'
        $sourcePath = Escape-NsisLiteral `
            (Join-Path $Directory $relativeWindows) 'payload source path'
        $lines.Add("File `"/oname=$fileName`" `"$sourcePath`"")
    }
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText(
        $IncludePath, (($lines -join "`r`n") + "`r`n"), $encoding)
}

function Write-UninstallInclude(
    [object[]]$Records,
    [string]$IncludePath
) {
    $lines = New-Object System.Collections.Generic.List[string]
    $directories = New-Object 'System.Collections.Generic.HashSet[string]' (
        [System.StringComparer]::Ordinal)
    foreach ($record in $Records) {
        $relative = ([string]$record.path).Replace('/', '\')
        $escaped = Escape-NsisLiteral $relative 'uninstall payload path'
        $lines.Add("Delete `"`$INSTDIR\$escaped`"")
        $parent = [System.IO.Path]::GetDirectoryName($relative)
        while (-not [string]::IsNullOrWhiteSpace($parent)) {
            [void]$directories.Add($parent)
            $parent = [System.IO.Path]::GetDirectoryName($parent)
        }
    }
    $orderedDirectories = [string[]]@($directories)
    [Array]::Sort($orderedDirectories, [System.StringComparer]::Ordinal)
    [Array]::Reverse($orderedDirectories)
    foreach ($directory in $orderedDirectories) {
        $escaped = Escape-NsisLiteral $directory 'uninstall payload directory'
        $lines.Add("RMDir `"`$INSTDIR\$escaped`"")
    }
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText(
        $IncludePath, (($lines -join "`r`n") + "`r`n"), $encoding)
}

function Four-Part-Version([string]$Value) {
    $parts = @($Value.Split('.'))
    if ($parts.Count -lt 1 -or $parts.Count -gt 4) {
        throw "Version must contain one to four numeric fields: $Value"
    }
    $normalized = New-Object System.Collections.Generic.List[string]
    foreach ($part in $parts) {
        $number = 0
        if (-not [int]::TryParse($part, [ref]$number) -or
            $number -lt 0 -or $number -gt 65535) {
            throw "Invalid installer version field in: $Value"
        }
        $normalized.Add($number.ToString())
    }
    while ($normalized.Count -lt 4) { $normalized.Add('0') }
    $normalized -join '.'
}

$source = Resolve-Local $SourceDir
$output = Resolve-Local $OutputPath
if ([string]::IsNullOrWhiteSpace($ManifestPath)) {
    $ManifestPath = [System.IO.Path]::ChangeExtension($output, '.manifest.json')
}
$manifestPathFull = Resolve-Local $ManifestPath
Assert-BuildPath $source 'SourceDir'
Assert-BuildPath $output 'OutputPath'
Assert-BuildPath $manifestPathFull 'ManifestPath'
if (-not (Test-Path -LiteralPath $source -PathType Container)) {
    throw "Installer source directory not found: $source"
}
if ($output.StartsWith(
        $source.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "OutputPath must not be inside SourceDir"
}
if ($manifestPathFull.Equals($output, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "ManifestPath must differ from OutputPath"
}
if ($manifestPathFull.StartsWith(
        $source.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "ManifestPath must not be inside SourceDir"
}

$makensisPath = Resolve-Makensis $Makensis
$toolVersionText = ((& $makensisPath /VERSION) -join '').Trim()
if ($LASTEXITCODE -ne 0 -or $toolVersionText -notmatch '^v(\d+)\.(\d+)') {
    throw "Could not read the makensis version"
}
$toolMajor = [int]$Matches[1]
$toolMinor = [int]$Matches[2]
if ($toolMajor -lt 3 -or ($toolMajor -eq 3 -and $toolMinor -lt 12)) {
    throw "NSIS 3.12+ is required; found $toolVersionText"
}
$fourPartVersion = Four-Part-Version $Version

$previousSourceDateEpoch = $env:SOURCE_DATE_EPOCH
$payloadInclude = "$output.payload.nsh"
$uninstallInclude = "$output.uninstall.nsh"
Push-Location $root
try {
    $worktree = Get-GitWorktreeState
    $status = $worktree.Status
    $dirty = $worktree.Dirty
    if ($dirty -and -not $AllowDirty) {
        throw "Installer build requires a clean Git worktree: $status (use -AllowDirty only for development proofs)"
    }
    $commit = (& git rev-parse HEAD).Trim()
    $commitTime = (& git show -s --format=%cI HEAD).Trim()
    $env:SOURCE_DATE_EPOCH = (& git show -s --format=%ct HEAD).Trim()

    & (Join-Path $PSScriptRoot 'validate_windows_dependencies.ps1') `
        -BundleDir $source `
        -EntryPoints @('Witness Game.exe') `
        -OutputPath (Join-Path $source 'windows-dependencies.json')
    if ($LASTEXITCODE -ne 0) { throw "Installer payload dependency validation failed" }

    $payload = @(Payload-Inventory $source)
    if (-not ($payload | Where-Object { $_.path -eq 'Witness Game.exe' })) {
        throw "Installer payload is missing Witness Game.exe"
    }
    if (-not ($payload | Where-Object { $_.path -eq 'windows-dependencies.json' })) {
        throw "Installer payload is missing windows-dependencies.json"
    }

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $output) | Out-Null
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $manifestPathFull) | Out-Null
    Remove-Item -LiteralPath $output, $manifestPathFull, $payloadInclude, $uninstallInclude `
        -Force -ErrorAction SilentlyContinue
    Write-PayloadInclude $source $payload $payloadInclude
    Write-UninstallInclude $payload $uninstallInclude

    & $makensisPath /V2 `
        "/DSTAGE_DIR=$source" `
        "/DOUTPUT_FILE=$output" `
        "/DPAYLOAD_INCLUDE=$payloadInclude" `
        "/DUNINSTALL_INCLUDE=$uninstallInclude" `
        "/DPRODUCT_VERSION=$fourPartVersion" `
        (Join-Path $root 'packaging/WitnessGame.nsi')
    if ($LASTEXITCODE -ne 0) {
        throw "makensis failed with exit code $LASTEXITCODE"
    }
    if (-not (Test-Path -LiteralPath $output -PathType Leaf)) {
        throw "makensis did not produce: $output"
    }

    $signature = Get-AuthenticodeSignature -LiteralPath $output
    $signer = if ($signature.SignerCertificate) {
        [ordered]@{
            subject = $signature.SignerCertificate.Subject
            thumbprint = $signature.SignerCertificate.Thumbprint.ToLowerInvariant()
        }
    } else {
        $null
    }
    $manifest = [ordered]@{
        schema = 1
        engineCommit = $commit
        dirty = $dirty
        generatedAtUtc = ([DateTimeOffset]::Parse($commitTime).UtcDateTime.ToString('o'))
        product = 'Witness Game'
        version = $Version
        tool = [ordered]@{
            name = 'NSIS'
            version = $toolVersionText
        }
        entryPoint = 'Witness Game.exe'
        installer = File-Record $output ([System.IO.Path]::GetFileName($output))
        authenticode = [ordered]@{
            status = $signature.Status.ToString()
            signer = $signer
        }
        payload = $payload
    }
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText(
        $manifestPathFull,
        (($manifest | ConvertTo-Json -Depth 10).TrimEnd() + "`r`n"),
        $encoding)

    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'verify_witness_installer.ps1') `
        -Destination (Join-Path (Split-Path -Parent $output) 'verify_witness_installer.ps1') `
        -Force

    if (-not $SkipVerify) {
        & (Join-Path $PSScriptRoot 'verify_witness_installer.ps1') `
            -ManifestPath $manifestPathFull
        if ($LASTEXITCODE -ne 0) { throw "Installer verification failed" }
    }

    Write-Host "WITNESS INSTALLER READY: $output"
    Write-Host "  commit: $commit  dirty: $dirty"
    Write-Host "  sha256: $($manifest.installer.sha256)"
    Write-Host "  Authenticode: $($manifest.authenticode.status)"
} finally {
    Remove-Item -LiteralPath $payloadInclude, $uninstallInclude `
        -Force -ErrorAction SilentlyContinue
    $env:SOURCE_DATE_EPOCH = $previousSourceDateEpoch
    Pop-Location
}
