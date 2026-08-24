[CmdletBinding()]
param(
    [string]$SourceDir = 'build/release/editor-v1/stage',
    [string]$OutputPath = 'build/release/editor-v1/SaidaEngine-Setup.exe',
    [string]$ManifestPath = '',
    [string]$Makensis = '',
    [string]$Version = '1.0.0-beta.4',
    [switch]$AllowDirty,
    [switch]$SkipVerify
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$arguments = @{
    SourceDir = $SourceDir
    OutputPath = $OutputPath
    ManifestPath = $ManifestPath
    Makensis = $Makensis
    Version = '1.0.0'
    DisplayVersion = $Version
    ProductName = 'SaidaEngine'
    EntryPoint = 'SaidaEngineHub.exe'
    DependencyEntryPoints = @(
        'SaidaEngine.exe', 'SaidaEngineHub.exe',
        'SaidaEngineRuntime.exe', 'saida_tool.exe'
    )
    InstallerScript = 'packaging/SaidaEngine.nsi'
    VerifierScript = 'tools/verify_editor_installer.ps1'
}
if ($AllowDirty) { $arguments.AllowDirty = $true }
if ($SkipVerify) { $arguments.SkipVerify = $true }

& (Join-Path $PSScriptRoot 'build_witness_installer.ps1') @arguments
