[CmdletBinding()]
param(
    [string]$ManifestPath = 'build/release/witness-installer/WitnessGame-Setup.manifest.json',
    [int]$TimeoutSeconds = 120,
    [switch]$RunWitness,
    [switch]$KeepWork
)

# Install an NSIS artifact silently into an isolated directory, verify the exact
# payload, optionally run Witness twice, then execute the shipped uninstaller.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-SafeRelativePath(
    [string]$Path,
    [string]$Label,
    [switch]$SingleName
) {
    if ([string]::IsNullOrWhiteSpace($Path) -or
        $Path.Contains('\') -or
        $Path.Contains(':') -or
        $Path.StartsWith('/') -or
        [System.IO.Path]::IsPathRooted($Path)) {
        throw "$Label is not a canonical relative path: $Path"
    }
    $segments = @($Path.Split('/'))
    if ($SingleName -and $segments.Count -ne 1) {
        throw "$Label must be a single filename: $Path"
    }
    foreach ($segment in $segments) {
        if ([string]::IsNullOrWhiteSpace($segment) -or
            $segment -eq '.' -or $segment -eq '..') {
            throw "$Label contains an unsafe path segment: $Path"
        }
    }
}

function Invoke-Process(
    [string]$FilePath,
    [string[]]$Arguments,
    [string]$WorkingDirectory,
    [string]$Label
) {
    $process = Start-Process -FilePath $FilePath -ArgumentList $Arguments `
        -WorkingDirectory $WorkingDirectory -PassThru -WindowStyle Hidden
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        throw "$Label timed out after $TimeoutSeconds seconds"
    }
    $process.Refresh()
    if ($process.ExitCode -ne 0) {
        throw "$Label exited with code $($process.ExitCode)"
    }
}

function Invoke-WitnessRun(
    [string]$Exe,
    [string]$WorkingDirectory,
    [string]$LogPrefix
) {
    $stdout = "$LogPrefix.out.log"
    $stderr = "$LogPrefix.err.log"
    Remove-Item -LiteralPath $stdout, $stderr -Force -ErrorAction SilentlyContinue
    $process = Start-Process -FilePath $Exe `
        -ArgumentList @('--test-autoload', 'E2EDriver=scripts/e2e_driver.js') `
        -WorkingDirectory $WorkingDirectory -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr -PassThru -WindowStyle Hidden
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        throw "Witness runtime timed out after $TimeoutSeconds seconds"
    }
    $process.Refresh()
    $exitCode = $process.ExitCode
    $text = ((Get-Content -Raw $stdout -ErrorAction SilentlyContinue) + "`n" +
             (Get-Content -Raw $stderr -ErrorAction SilentlyContinue))
    if ($null -ne $exitCode -and $exitCode -ne 0) {
        throw "Witness runtime exited with code ${exitCode}:`n$text"
    }
    return $text
}

$manifestPathFull = [System.IO.Path]::GetFullPath($ManifestPath)
if (-not (Test-Path -LiteralPath $manifestPathFull -PathType Leaf)) {
    throw "Installer manifest not found: $manifestPathFull"
}
$manifest = Get-Content -Raw -LiteralPath $manifestPathFull | ConvertFrom-Json
if ($manifest.schema -ne 1) { throw "Unsupported installer manifest schema" }
Assert-SafeRelativePath ([string]$manifest.installer.path) 'installer.path' -SingleName
Assert-SafeRelativePath ([string]$manifest.entryPoint) 'entryPoint'
if ([string]$manifest.installer.sha256 -notmatch '^[0-9a-f]{64}$' -or
    [long]$manifest.installer.bytes -lt 0) {
    throw "Invalid installer hash/size record"
}

$bundle = Split-Path -Parent $manifestPathFull
$installer = Join-Path $bundle $manifest.installer.path
if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) {
    throw "Installer not found: $installer"
}
$installerFile = Get-Item -LiteralPath $installer
if ($installerFile.Length -ne $manifest.installer.bytes) {
    throw "Installer size mismatch"
}
$installerHash =
    (Get-FileHash -Algorithm SHA256 -LiteralPath $installer).Hash.ToLowerInvariant()
if ($installerHash -ne [string]$manifest.installer.sha256) {
    throw "Installer SHA-256 mismatch"
}

$work = Join-Path $bundle '.verify-installer'
$workFull = [System.IO.Path]::GetFullPath($work)
$bundleFull = [System.IO.Path]::GetFullPath($bundle).TrimEnd('\', '/')
if (-not $workFull.StartsWith(
        $bundleFull + [System.IO.Path]::DirectorySeparatorChar,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Unsafe installer verification path: $workFull"
}
if (Test-Path -LiteralPath $workFull) {
    Remove-Item -LiteralPath $workFull -Recurse -Force
}
$installDir = Join-Path $workFull 'installed'
$saveDir = Join-Path $workFull 'saves'
New-Item -ItemType Directory -Path $workFull | Out-Null

$previousSaveDir = $env:SAIDA_SAVE_DIR
$previousHidden = $env:SAIDA_WINDOW_HIDDEN
try {
    Invoke-Process $installer @('/S', "/D=$installDir") $workFull 'installer'
    if (-not (Test-Path -LiteralPath $installDir -PathType Container)) {
        throw "Installer did not create: $installDir"
    }

    $expected = New-Object 'System.Collections.Generic.Dictionary[string,object]' (
        [System.StringComparer]::Ordinal)
    $caseGuard = New-Object 'System.Collections.Generic.HashSet[string]' (
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($record in $manifest.payload) {
        $relative = [string]$record.path
        Assert-SafeRelativePath $relative 'payload.path'
        if (-not $caseGuard.Add($relative)) {
            throw "Manifest payload contains case-colliding paths: $relative"
        }
        if ([string]$record.sha256 -notmatch '^[0-9a-f]{64}$' -or
            [long]$record.bytes -lt 0) {
            throw "Invalid payload hash/size record: $relative"
        }
        $expected.Add($relative, $record)
    }
    if (-not $expected.ContainsKey([string]$manifest.entryPoint)) {
        throw "Manifest entryPoint is absent from the payload"
    }
    $actual = New-Object 'System.Collections.Generic.HashSet[string]' (
        [System.StringComparer]::Ordinal)
    $prefix = $installDir.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    foreach ($file in Get-ChildItem -LiteralPath $installDir -Recurse -File) {
        $relative = $file.FullName.Substring($prefix.Length).Replace('\', '/')
        [void]$actual.Add($relative)
        if ($relative -eq 'Uninstall.exe') { continue }
        if (-not $expected.ContainsKey($relative)) {
            throw "Unexpected installed file: $relative"
        }
        $record = $expected[$relative]
        if ($file.Length -ne $record.bytes) {
            throw "Installed size mismatch: $relative"
        }
        $hash =
            (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash.ToLowerInvariant()
        if ($hash -ne [string]$record.sha256) {
            throw "Installed SHA-256 mismatch: $relative"
        }
    }
    foreach ($relative in $expected.Keys) {
        if (-not $actual.Contains($relative)) {
            throw "Missing installed file: $relative"
        }
    }
    if (-not $actual.Contains('Uninstall.exe')) {
        throw "Installer did not create Uninstall.exe"
    }
    if ($actual.Count -ne $expected.Count + 1) {
        throw "Installed inventory is not exact"
    }
    if (Test-Path -LiteralPath (Join-Path $installDir 'saves')) {
        throw "Installer payload unexpectedly contains saves"
    }

    if ($RunWitness) {
        $env:SAIDA_SAVE_DIR = $saveDir
        $env:SAIDA_WINDOW_HIDDEN = '1'
        $exe = Join-Path $installDir $manifest.entryPoint
        $first = Invoke-WitnessRun $exe $installDir (Join-Path $workFull 'first-run')
        if ($first -notmatch '\[E2E\] PASS') {
            throw "Installed Witness first run did not report E2E PASS:`n$first"
        }
        $restart = Invoke-WitnessRun $exe $installDir (Join-Path $workFull 'restart')
        if ($restart -notmatch '\[E2E\] RESTART PASS') {
            throw "Installed Witness restart did not report RESTART PASS:`n$restart"
        }
    }

    $uninstaller = Join-Path $installDir 'Uninstall.exe'
    Invoke-Process $uninstaller @('/S') $installDir 'uninstaller'
    for ($i = 0; $i -lt 100 -and (Test-Path -LiteralPath $installDir); ++$i) {
        Start-Sleep -Milliseconds 100
    }
    if (Test-Path -LiteralPath $installDir) {
        throw "Uninstaller did not remove the installation directory"
    }

    Write-Host "WITNESS INSTALLER VERIFY PASS"
    Write-Host "  installer sha256: $installerHash"
    Write-Host "  payload files: $($expected.Count)"
    Write-Host "  install/uninstall: PASS"
    if ($RunWitness) { Write-Host "  gameplay + restart: PASS" }
} catch {
    $KeepWork = $true
    throw
} finally {
    if ($null -eq $previousSaveDir) {
        Remove-Item Env:SAIDA_SAVE_DIR -ErrorAction SilentlyContinue
    } else {
        $env:SAIDA_SAVE_DIR = $previousSaveDir
    }
    if ($null -eq $previousHidden) {
        Remove-Item Env:SAIDA_WINDOW_HIDDEN -ErrorAction SilentlyContinue
    } else {
        $env:SAIDA_WINDOW_HIDDEN = $previousHidden
    }
    if (-not $KeepWork -and (Test-Path -LiteralPath $workFull)) {
        Remove-Item -LiteralPath $workFull -Recurse -Force
    } elseif (Test-Path -LiteralPath $workFull) {
        Write-Host "Installer verification files kept in $workFull"
    }
}
