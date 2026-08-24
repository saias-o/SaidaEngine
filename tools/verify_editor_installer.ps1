[CmdletBinding()]
param(
    [string]$ManifestPath = 'build/release/editor-v1/SaidaEngine-Setup.manifest.json',
    [int]$TimeoutSeconds = 180,
    [switch]$KeepWork
)

# Clean-install proof for the published editor. The generic verifier first
# proves the installer payload byte-for-byte and proves uninstall. This script
# then installs a second time and exercises only files present in that install:
# Hub boot, editor runtime contract, CLI identity, editor Build of the bundled
# Witness sample, exported-game run + restart, and clean uninstall.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Invoke-QuietProcess(
    [string]$FilePath,
    [string[]]$Arguments,
    [string]$WorkingDirectory,
    [string]$Label
) {
    foreach ($argument in $Arguments) {
        if ($argument.Contains('"')) { throw "$Label contains an unsafe quote" }
    }
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $FilePath
    # NSIS parses /D= from the raw command line and requires it to be the last
    # token; quoting the whole token makes it silently use the default folder.
    # Its /D value consumes the remainder, so a path containing spaces is safe.
    $info.Arguments = (($Arguments | ForEach-Object {
        if ($_.StartsWith('/D=', [System.StringComparison]::OrdinalIgnoreCase)) {
            $_
        } elseif ($_ -match '\s') {
            '"' + $_ + '"'
        } else {
            $_
        }
    }) -join ' ')
    $info.WorkingDirectory = $WorkingDirectory
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $info
    if (-not $process.Start()) { throw "$Label could not start" }
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        $process.Kill()
        throw "$Label timed out after $TimeoutSeconds seconds"
    }
    $exitCode = $process.ExitCode
    if ($null -eq $exitCode -or $exitCode -ne 0) {
        throw "$Label exited with code $exitCode"
    }
}

$manifestPathFull = [System.IO.Path]::GetFullPath($ManifestPath)
if (-not (Test-Path -LiteralPath $manifestPathFull -PathType Leaf)) {
    throw "Editor installer manifest not found: $manifestPathFull"
}

# Reuse the exact-inventory verifier rather than maintaining a second copy of
# its path, hash, case-collision and uninstall guards.
& (Join-Path $PSScriptRoot 'verify_witness_installer.ps1') `
    -ManifestPath $manifestPathFull -TimeoutSeconds $TimeoutSeconds

$manifest = Get-Content -Raw -LiteralPath $manifestPathFull | ConvertFrom-Json
if ($manifest.product -ne 'SaidaEngine') {
    throw "Expected a SaidaEngine installer manifest"
}
$bundle = Split-Path -Parent $manifestPathFull
$installer = Join-Path $bundle ([string]$manifest.installer.path)
$work = Join-Path $bundle '.verify-editor-installer'
$workFull = [System.IO.Path]::GetFullPath($work)
$bundleFull = [System.IO.Path]::GetFullPath($bundle).TrimEnd('\', '/')
if (-not $workFull.StartsWith(
        $bundleFull + [System.IO.Path]::DirectorySeparatorChar,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Unsafe editor verification path: $workFull"
}
if (Test-Path -LiteralPath $workFull) {
    Remove-Item -LiteralPath $workFull -Recurse -Force
}
$installDir = Join-Path $workFull 'installed'
New-Item -ItemType Directory -Path $workFull | Out-Null

try {
    Invoke-QuietProcess $installer @('/S', "/D=$installDir") $workFull 'editor installer'
    foreach ($required in @(
        'SaidaEngine.exe', 'SaidaEngineHub.exe', 'SaidaEngineRuntime.exe',
        'saida_tool.exe', 'glfw3.dll', 'vulkan-1.dll', 'saida-install.json')) {
        if (-not (Test-Path -LiteralPath (Join-Path $installDir $required) -PathType Leaf)) {
            throw "Installed editor is missing $required"
        }
    }

    & (Join-Path $PSScriptRoot 'verify_editor_layout.ps1') `
        -InstallDir $installDir -WorkDir (Join-Path $workFull 'layout-work') `
        -TimeoutSeconds $TimeoutSeconds

    Invoke-QuietProcess (Join-Path $installDir 'Uninstall.exe') @('/S') `
        $installDir 'editor uninstaller'
    for ($i = 0; $i -lt 100 -and (Test-Path -LiteralPath $installDir); ++$i) {
        Start-Sleep -Milliseconds 100
    }
    if (Test-Path -LiteralPath $installDir) {
        throw "Editor uninstaller did not remove the installation directory"
    }

    Write-Host 'SAIDAENGINE EDITOR INSTALLER VERIFY PASS'
    Write-Host '  exact payload + uninstall: PASS'
    Write-Host '  Hub + editor + CLI: PASS'
    Write-Host '  editor Build + exported game restart: PASS'
} catch {
    $KeepWork = $true
    throw
} finally {
    if (-not $KeepWork -and (Test-Path -LiteralPath $workFull)) {
        Remove-Item -LiteralPath $workFull -Recurse -Force
    } elseif (Test-Path -LiteralPath $workFull) {
        Write-Host "Editor verification files kept in $workFull"
    }
}
