[CmdletBinding()]
param(
    [string]$BundleDir = $PSScriptRoot,
    [int]$TimeoutSeconds = 90,
    [switch]$KeepWork
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-ChildPath([string]$Path, [string]$Root) {
    $full = [System.IO.Path]::GetFullPath($Path)
    $rootFull = [System.IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    if (-not $full.StartsWith($rootFull + [System.IO.Path]::DirectorySeparatorChar,
                             [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Unsafe verification path outside bundle: $full"
    }
    return $full
}

function Invoke-WitnessRun([string]$Exe, [string]$WorkingDir, [string]$LogPrefix) {
    $stdout = "$LogPrefix.out.log"
    $stderr = "$LogPrefix.err.log"
    Remove-Item -LiteralPath $stdout, $stderr -Force -ErrorAction SilentlyContinue
    $process = Start-Process -FilePath $Exe `
        -ArgumentList @('--test-autoload', 'E2EDriver=scripts/e2e_driver.js') `
        -WorkingDirectory $WorkingDir -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr -PassThru
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        throw "WitnessGame timed out after $TimeoutSeconds seconds"
    }
    $text = ((Get-Content -Raw $stdout -ErrorAction SilentlyContinue) + "`n" +
             (Get-Content -Raw $stderr -ErrorAction SilentlyContinue))
    $process.Refresh()
    $exitCode = $process.ExitCode
    if ($null -ne $exitCode -and $exitCode -ne 0) {
        throw "WitnessGame exited with code ${exitCode}:`n$text"
    }
    return $text
}

$bundle = [System.IO.Path]::GetFullPath($BundleDir)
$manifestPath = Join-Path $bundle 'release-manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "Missing release-manifest.json in $bundle"
}
$manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
if ($manifest.schema -ne 1) { throw "Unsupported release manifest schema" }

$artifact = $manifest.artifacts.windows
$archive = Join-Path $bundle $artifact.archive
if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) {
    throw "Missing Windows archive: $archive"
}
$actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
if ($actualHash -ne [string]$artifact.sha256) {
    throw "Windows archive checksum mismatch: expected $($artifact.sha256), got $actualHash"
}

$work = Assert-ChildPath (Join-Path $bundle '.verify-windows') $bundle
if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
New-Item -ItemType Directory -Path $work | Out-Null

try {
    Expand-Archive -LiteralPath $archive -DestinationPath $work
    if (Test-Path -LiteralPath (Join-Path $work 'saves')) {
        throw "Release archive must not contain player saves"
    }
    $exe = Join-Path $work $artifact.entryPoint
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
        throw "Missing Windows entry point: $($artifact.entryPoint)"
    }

    # Shipped games persist saves under the per-user OS data directory. Pin them
    # to a fresh per-invocation directory (shared by both runs) so the restart
    # proof is hermetic and cannot be polluted by a prior run's saves. Absent
    # this override the runtime would write to %APPDATA%\SaidaEngine\Games\...,
    # which persists across invocations.
    $env:SAIDA_SAVE_DIR = Join-Path $work '.saves'

    $first = Invoke-WitnessRun $exe $work (Join-Path $work 'first-run')
    if ($first -notmatch '\[E2E\] PASS') {
        throw "First run did not report E2E PASS:`n$first"
    }
    $restart = Invoke-WitnessRun $exe $work (Join-Path $work 'restart')
    if ($restart -notmatch '\[E2E\] RESTART PASS') {
        throw "Second run did not report RESTART PASS:`n$restart"
    }

    Write-Host "WINDOWS CLEAN-BUNDLE PASS"
    Write-Host "  archive sha256: $actualHash"
    Write-Host "  gameplay/UI: PASS"
    Write-Host "  save/UI restart: PASS"
} catch {
    $KeepWork = $true
    throw
} finally {
    Remove-Item Env:SAIDA_SAVE_DIR -ErrorAction SilentlyContinue
    if (-not $KeepWork -and (Test-Path -LiteralPath $work)) {
        Remove-Item -LiteralPath $work -Recurse -Force
    } elseif (Test-Path -LiteralPath $work) {
        Write-Host "Verification files kept in $work"
    }
}
