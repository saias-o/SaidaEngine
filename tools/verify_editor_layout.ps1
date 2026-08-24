[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InstallDir,
    [string]$WorkDir = '',
    [int]$TimeoutSeconds = 180,
    [switch]$KeepWork
)

# Exercises an editor tree without using anything from the source checkout.
# Both the ZIP verifier and the installer verifier use this same contract.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Invoke-CapturedProcess(
    [string]$FilePath,
    [string[]]$Arguments,
    [string]$WorkingDirectory,
    [string]$LogPrefix,
    [string]$Label
) {
    $stdout = "$LogPrefix.out.log"
    $stderr = "$LogPrefix.err.log"
    Remove-Item -LiteralPath $stdout, $stderr -Force -ErrorAction SilentlyContinue
    foreach ($argument in $Arguments) {
        if ($argument.Contains('"')) { throw "$Label contains an unsafe quote" }
    }
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $FilePath
    $info.Arguments = (($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' ')
    $info.WorkingDirectory = $WorkingDirectory
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $info
    if (-not $process.Start()) { throw "$Label could not start" }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        $process.Kill()
        throw "$Label timed out after $TimeoutSeconds seconds"
    }
    $exitCode = $process.ExitCode
    $stdoutText = $stdoutTask.GetAwaiter().GetResult()
    $stderrText = $stderrTask.GetAwaiter().GetResult()
    [System.IO.File]::WriteAllText($stdout, $stdoutText)
    [System.IO.File]::WriteAllText($stderr, $stderrText)
    $text = $stdoutText + "`n" + $stderrText
    if ($null -eq $exitCode -or $exitCode -ne 0) {
        throw "$Label exited with code ${exitCode}:`n$text"
    }
    $text
}

$install = [System.IO.Path]::GetFullPath($InstallDir)
if (-not (Test-Path -LiteralPath $install -PathType Container)) {
    throw "Editor layout not found: $install"
}
if ([string]::IsNullOrWhiteSpace($WorkDir)) {
    $WorkDir = Join-Path (Split-Path -Parent $install) '.verify-editor-layout'
}
$work = [System.IO.Path]::GetFullPath($WorkDir)
if ($work.Equals($install, [System.StringComparison]::OrdinalIgnoreCase) -or
    $work.StartsWith($install.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar,
                    [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'WorkDir must not be inside the installed editor layout'
}
if (Test-Path -LiteralPath $work) {
    Remove-Item -LiteralPath $work -Recurse -Force
}
New-Item -ItemType Directory -Path $work | Out-Null

foreach ($required in @(
    'SaidaEngine.exe', 'SaidaEngineHub.exe', 'SaidaEngineRuntime.exe',
    'saida_tool.exe', 'glfw3.dll', 'vulkan-1.dll', 'saida-install.json',
    'assets', 'shaders', 'samples\WitnessGame')) {
    if (-not (Test-Path -LiteralPath (Join-Path $install $required))) {
        throw "Editor layout is missing $required"
    }
}

$previousHidden = $env:SAIDA_WINDOW_HIDDEN
$previousSave = $env:SAIDA_SAVE_DIR
$previousState = $env:SAIDA_STATE_DIR
$previousProjects = $env:SAIDA_PROJECTS_DIR
try {
    $env:SAIDA_WINDOW_HIDDEN = '1'
    $env:SAIDA_SAVE_DIR = Join-Path $work 'saves'
    $env:SAIDA_STATE_DIR = Join-Path $work 'state'
    $env:SAIDA_PROJECTS_DIR = Join-Path $work 'projects'

    $tool = Invoke-CapturedProcess (Join-Path $install 'saida_tool.exe') `
        @('describe-engine') $install (Join-Path $work 'tool') 'installed saida_tool'
    if ($tool -notmatch 'engineVersion') {
        throw "Installed saida_tool did not describe the engine:`n$tool"
    }

    Invoke-CapturedProcess (Join-Path $install 'SaidaEngineHub.exe') `
        @('--verify-installation') $install (Join-Path $work 'hub') `
        'installed Hub verification' | Out-Null

    $contract = Invoke-CapturedProcess (Join-Path $install 'SaidaEngine.exe') `
        @('--verify-runtime-contract') $install (Join-Path $work 'editor') `
        'installed editor runtime contract'
    if ($contract -notmatch '\[CONTRACT\] PASS') {
        throw "Installed editor did not report its runtime contract:`n$contract"
    }

    $project = Join-Path $work 'project'
    Copy-Item -LiteralPath (Join-Path $install 'samples\WitnessGame') `
        -Destination $project -Recurse
    Remove-Item -LiteralPath (Join-Path $project 'saves') -Recurse -Force `
        -ErrorAction SilentlyContinue

    $export = Join-Path $work 'export'
    $build = Invoke-CapturedProcess (Join-Path $install 'SaidaEngine.exe') `
        @('--project', (Join-Path $project 'WitnessGame.saidaproj'),
          '--build', $export) `
        $install (Join-Path $work 'build') 'installed editor Build'
    if ($build -notmatch '\[BUILD\] PASS') {
        throw "Installed editor Build did not report PASS:`n$build"
    }

    $game = Join-Path $export 'Witness Game.exe'
    $first = Invoke-CapturedProcess $game `
        @('--test-autoload', 'E2EDriver=scripts/e2e_driver.js') `
        $export (Join-Path $work 'game-first') 'exported game first run'
    if ($first -notmatch '\[E2E\] PASS') {
        throw "Exported game first run did not report PASS:`n$first"
    }
    $restart = Invoke-CapturedProcess $game `
        @('--test-autoload', 'E2EDriver=scripts/e2e_driver.js') `
        $export (Join-Path $work 'game-restart') 'exported game restart'
    if ($restart -notmatch '\[E2E\] RESTART PASS') {
        throw "Exported game restart did not report PASS:`n$restart"
    }

    Write-Host 'SAIDAENGINE EDITOR LAYOUT VERIFY PASS'
    Write-Host '  Hub + editor + CLI: PASS'
    Write-Host '  editor Build + exported game restart: PASS'
} catch {
    $KeepWork = $true
    throw
} finally {
    foreach ($entry in @(
        @{ Name = 'SAIDA_WINDOW_HIDDEN'; Value = $previousHidden },
        @{ Name = 'SAIDA_SAVE_DIR'; Value = $previousSave },
        @{ Name = 'SAIDA_STATE_DIR'; Value = $previousState },
        @{ Name = 'SAIDA_PROJECTS_DIR'; Value = $previousProjects })) {
        if ($null -eq $entry.Value) {
            Remove-Item ("Env:" + $entry.Name) -ErrorAction SilentlyContinue
        } else {
            Set-Item ("Env:" + $entry.Name) $entry.Value
        }
    }
    if (-not $KeepWork -and (Test-Path -LiteralPath $work)) {
        Remove-Item -LiteralPath $work -Recurse -Force
    } elseif (Test-Path -LiteralPath $work) {
        Write-Host "Editor layout verification files kept in $work"
    }
}
