[CmdletBinding()]
param(
    [ValidateSet('Chrome', 'Edge')]
    [string]$Browser,
    [string]$BundleDir = $PSScriptRoot,
    [string]$BrowserPath,
    [string]$PythonPath = 'python',
    [int]$Port = 18080,
    [int]$TimeoutSeconds = 90,
    [switch]$Headless,
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

function Find-Browser([string]$Name) {
    $candidates = if ($Name -eq 'Chrome') {
        @(
            "$env:ProgramFiles\Google\Chrome\Application\chrome.exe",
            "${env:ProgramFiles(x86)}\Google\Chrome\Application\chrome.exe",
            "$env:LOCALAPPDATA\Google\Chrome\Application\chrome.exe"
        )
    } else {
        @(
            "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe",
            "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
            "$env:LOCALAPPDATA\Microsoft\Edge\Application\msedge.exe"
        )
    }
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return $candidate
        }
    }
    throw "$Name executable not found; pass -BrowserPath explicitly"
}

function Wait-Http([string]$Url, [int]$Seconds) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    do {
        try { return Invoke-WebRequest -UseBasicParsing -Uri $Url -TimeoutSec 2 }
        catch { Start-Sleep -Milliseconds 250 }
    } while ((Get-Date) -lt $deadline)
    throw "Timed out waiting for $Url"
}

function Wait-Verdict([string]$BaseUrl, [string]$Pattern, [int]$Seconds) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    do {
        try {
            $report = Invoke-RestMethod -Uri "$BaseUrl/__saida_e2e" -TimeoutSec 2
            if ([string]$report.verdict -match $Pattern) { return [string]$report.verdict }
            if ([string]$report.verdict -match '\[E2E\]\s+FAIL(?:\s|$)') {
                throw "Browser reported failure: $($report.verdict)"
            }
        } catch {
            if ($_.Exception.Message -like 'Browser reported failure:*') { throw }
        }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $deadline)
    throw "Timed out waiting for browser verdict $Pattern"
}

function Stop-BrowserProfile([string]$ProfilePath) {
    Get-CimInstance Win32_Process | Where-Object {
        $_.CommandLine -and $_.CommandLine.Contains($ProfilePath)
    } | ForEach-Object {
        Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
    }
}

if (-not $Browser) { throw "Pass -Browser Chrome or -Browser Edge" }
if (-not $BrowserPath) { $BrowserPath = Find-Browser $Browser }

$bundle = [System.IO.Path]::GetFullPath($BundleDir)
$manifestPath = Join-Path $bundle 'release-manifest.json'
$manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
if ($manifest.schema -ne 1) { throw "Unsupported release manifest schema" }
$artifact = $manifest.artifacts.web
$archive = Join-Path $bundle $artifact.archive
$actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
if ($actualHash -ne [string]$artifact.sha256) {
    throw "Web archive checksum mismatch: expected $($artifact.sha256), got $actualHash"
}

$work = Assert-ChildPath (Join-Path $bundle ".verify-web-$($Browser.ToLowerInvariant())") $bundle
if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
New-Item -ItemType Directory -Path $work | Out-Null
$profile = Assert-ChildPath (Join-Path $work 'browser-profile') $work
$server = $null

try {
    Expand-Archive -LiteralPath $archive -DestinationPath $work
    if (Test-Path -LiteralPath (Join-Path $work 'project/saves')) {
        throw "Release archive must not contain player saves"
    }

    $serverOut = Join-Path $work 'server.out.log'
    $serverErr = Join-Path $work 'server.err.log'
    $server = Start-Process -FilePath $PythonPath `
        -ArgumentList @('serve.py', '.', [string]$Port) -WorkingDirectory $work `
        -WindowStyle Hidden -RedirectStandardOutput $serverOut `
        -RedirectStandardError $serverErr -PassThru

    $baseUrl = "http://127.0.0.1:$Port"
    $index = Wait-Http "$baseUrl/index.html" 10
    if ($index.Headers['Cross-Origin-Opener-Policy'] -ne 'same-origin' -or
        $index.Headers['Cross-Origin-Embedder-Policy'] -ne 'require-corp') {
        throw "Web server did not return the required COOP/COEP headers"
    }
    $wasm = Invoke-WebRequest -UseBasicParsing -Method Head -Uri "$baseUrl/index.wasm"
    if ([string]$wasm.Headers['Content-Type'] -notmatch 'application/wasm') {
        throw "index.wasm has the wrong Content-Type: $($wasm.Headers['Content-Type'])"
    }

    $encodedAutoload = [uri]::EscapeDataString('E2EDriver=scripts/e2e_driver.js')
    $url = "$baseUrl/?smoke&report&test-autoload=$encodedAutoload"
    $arguments = @(
        "--user-data-dir=$profile", '--no-first-run', '--no-default-browser-check',
        '--disable-background-networking', '--new-window', $url
    )
    if ($Headless) { $arguments = @('--headless=new') + $arguments }
    Start-Process -FilePath $BrowserPath -ArgumentList $arguments | Out-Null
    $first = Wait-Verdict $baseUrl '\[E2E\] PASS' $TimeoutSeconds

    # A genuine second process proves the previous PASS was durably flushed to
    # IndexedDB. `--new-tab` does not reliably navigate an existing headless
    # Chrome instance that is already using this profile.
    Stop-BrowserProfile $profile
    Start-Sleep -Milliseconds 500
    Invoke-WebRequest -UseBasicParsing -Method Post -Uri "$baseUrl/__saida_e2e/reset" | Out-Null
    Start-Process -FilePath $BrowserPath -ArgumentList $arguments | Out-Null
    $restart = Wait-Verdict $baseUrl '\[E2E\] RESTART PASS' $TimeoutSeconds

    Write-Host "$Browser WEB PASS"
    Write-Host "  archive sha256: $actualHash"
    Write-Host "  headers: COOP/COEP + application/wasm"
    Write-Host "  first run: $first"
    Write-Host "  restart: $restart"
} catch {
    $KeepWork = $true
    throw
} finally {
    if ($server -and -not $server.HasExited) {
        Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $profile) {
        Stop-BrowserProfile $profile
    }
    if (-not $KeepWork -and (Test-Path -LiteralPath $work)) {
        Start-Sleep -Milliseconds 500
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    } elseif (Test-Path -LiteralPath $work) {
        Write-Host "Verification files kept in $work"
    }
}
