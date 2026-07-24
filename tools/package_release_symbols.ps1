[CmdletBinding()]
param(
    [string]$BuildDir = 'build',
    [string]$OutputDir = 'build/release/windows-symbols',
    [string]$Objcopy = '',
    [string]$Objdump = '',
    [switch]$AllowDirty
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $root 'build')).TrimEnd('\', '/')
. (Join-Path $PSScriptRoot 'git_worktree_state.ps1')
$build = if ([System.IO.Path]::IsPathRooted($BuildDir)) {
    [System.IO.Path]::GetFullPath($BuildDir)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $root $BuildDir))
}
$out = if ([System.IO.Path]::IsPathRooted($OutputDir)) {
    [System.IO.Path]::GetFullPath($OutputDir)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $root $OutputDir))
}
if (-not $out.StartsWith($buildRoot + [System.IO.Path]::DirectorySeparatorChar,
                         [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "OutputDir must stay under $buildRoot"
}

function Resolve-Tool([string]$Provided, [string[]]$Names) {
    if ($Provided) {
        $candidate = if ([System.IO.Path]::IsPathRooted($Provided)) {
            $Provided
        } else {
            (Get-Command $Provided -ErrorAction Stop).Source
        }
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            throw "Tool not found: $candidate"
        }
        return [System.IO.Path]::GetFullPath($candidate)
    }
    foreach ($name in $Names) {
        $command = Get-Command $name -ErrorAction SilentlyContinue
        if ($command) { return $command.Source }
    }
    throw "Required tool not found: $($Names -join ' or ')"
}

function Invoke-Tool([string]$Tool, [string[]]$Arguments) {
    & $Tool @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$([System.IO.Path]::GetFileName($Tool)) failed with exit code $LASTEXITCODE"
    }
}

function File-Record([string]$Path, [string]$Relative) {
    $file = Get-Item -LiteralPath $Path
    [ordered]@{
        path = $Relative.Replace('\', '/')
        bytes = $file.Length
        sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash.ToLowerInvariant()
    }
}

function Write-Utf8NoBom([string]$Path, [string]$Text) {
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, ($Text.TrimEnd() + "`r`n"), $encoding)
}

$previousSourceDateEpoch = $env:SOURCE_DATE_EPOCH
Push-Location $root
try {
    $worktree = Get-GitWorktreeState
    $status = $worktree.Status
    $dirty = $worktree.Dirty
    if ($dirty -and -not $AllowDirty) {
        throw "Symbol packaging requires a clean Git worktree: $status (use -AllowDirty only for development proofs)"
    }

    $objcopyPath = Resolve-Tool $Objcopy @('objcopy.exe', 'objcopy')
    $objdumpPath = Resolve-Tool $Objdump @('objdump.exe', 'objdump')
    $commit = (& git rev-parse HEAD).Trim()
    $commitTime = (& git show -s --format=%cI HEAD).Trim()
    $commitEpoch = (& git show -s --format=%ct HEAD).Trim()
    $toolVersion = ((& $objcopyPath --version | Select-Object -First 1) -join '').Trim()
    if ($LASTEXITCODE -ne 0) { throw "Could not read objcopy version" }
    # GNU objcopy writes the PE timestamp. SOURCE_DATE_EPOCH pins it to the
    # source commit so two packages of the same binaries remain byte-identical.
    $env:SOURCE_DATE_EPOCH = $commitEpoch

    $inputs = @(
        [ordered]@{ name = 'SaidaEngine'; path = (Join-Path $build 'bin/SaidaEngine.exe') },
        [ordered]@{ name = 'SaidaEngineRuntime'; path = (Join-Path $build 'bin/SaidaEngineRuntime.exe') },
        [ordered]@{ name = 'SaidaEngineHub'; path = (Join-Path $build 'bin/SaidaEngineHub.exe') },
        [ordered]@{ name = 'saida_tool'; path = (Join-Path $build 'bin/saida_tool.exe') }
    )
    foreach ($input in $inputs) {
        if (-not (Test-Path -LiteralPath $input.path -PathType Leaf)) {
            throw "Missing release binary: $($input.path)"
        }
    }

    if (Test-Path -LiteralPath $out) {
        Remove-Item -LiteralPath $out -Recurse -Force
    }
    $binaryDir = Join-Path $out 'binaries'
    $symbolDir = Join-Path $out 'symbols'
    New-Item -ItemType Directory -Path $binaryDir, $symbolDir | Out-Null

    $records = New-Object System.Collections.Generic.List[object]
    foreach ($input in $inputs) {
        $source = [System.IO.Path]::GetFullPath([string]$input.path)
        $fileName = [System.IO.Path]::GetFileName($source)
        $debugName = "$fileName.dbg"
        $binary = Join-Path $binaryDir $fileName
        $symbols = Join-Path $symbolDir $debugName
        Copy-Item -LiteralPath $source -Destination $binary

        Invoke-Tool $objcopyPath @('--only-keep-debug', $source, $symbols)
        Invoke-Tool $objcopyPath @('--strip-debug', $binary)
        Push-Location $symbolDir
        try {
            Invoke-Tool $objcopyPath @("--add-gnu-debuglink=$debugName", $binary)
        } finally {
            Pop-Location
        }

        $binarySections = (& $objdumpPath -h $binary 2>&1) -join "`n"
        if ($LASTEXITCODE -ne 0 -or $binarySections -notmatch '\.gnu_debuglink') {
            throw "Failed to embed .gnu_debuglink in $fileName"
        }
        $symbolSections = (& $objdumpPath -h $symbols 2>&1) -join "`n"
        if ($LASTEXITCODE -ne 0 -or $symbolSections -notmatch '\.debug_info') {
            throw "$fileName does not contain debug information; configure RelWithDebInfo"
        }

        $records.Add([ordered]@{
            name = [string]$input.name
            originalSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $source).Hash.ToLowerInvariant()
            binary = File-Record $binary "binaries/$fileName"
            symbols = File-Record $symbols "symbols/$debugName"
            debugLink = $debugName
        })
    }

    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'verify_release_symbols.ps1') `
        -Destination (Join-Path $out 'verify_release_symbols.ps1')
    $dependencyReportPath = Join-Path $out 'windows-dependencies.json'
    & (Join-Path $PSScriptRoot 'validate_windows_dependencies.ps1') `
        -BundleDir (Join-Path $build 'bin') `
        -OutputPath $dependencyReportPath
    if ($LASTEXITCODE -ne 0) { throw "Windows dependency validation failed" }
    $manifest = [ordered]@{
        schema = 1
        engineCommit = $commit
        dirty = $dirty
        generatedAtUtc = ([DateTimeOffset]::Parse($commitTime).UtcDateTime.ToString('o'))
        artifactName = "windows-symbols-$commit"
        tool = $toolVersion
        dependencyReport = File-Record $dependencyReportPath 'windows-dependencies.json'
        binaries = $records.ToArray()
    }
    $manifestPath = Join-Path $out 'windows-symbols-manifest.json'
    Write-Utf8NoBom $manifestPath ($manifest | ConvertTo-Json -Depth 10)

    & (Join-Path $out 'verify_release_symbols.ps1') `
        -BundleDir $out -ExpectedCommit $commit -Objdump $objdumpPath
    if ($LASTEXITCODE -ne 0) { throw "Symbol bundle verification failed" }

    Write-Host "WINDOWS SYMBOL BUNDLE READY: $out"
    Write-Host "  commit: $commit"
    Write-Host "  dirty: $dirty"
} finally {
    $env:SOURCE_DATE_EPOCH = $previousSourceDateEpoch
    Pop-Location
}
