# Diagnostic scene variants; the authored project and scene are left intact.
[CmdletBinding()]
param(
    [ValidateSet('NoAO', 'NoShadows', 'NoAOOrShadows')]
    [string]$Mode = 'NoAO',
    [switch]$PrepareOnly
)
$ErrorActionPreference = 'Stop'
$demoRoot = $PSScriptRoot
$engineExe = Join-Path $demoRoot '..\build-rel\bin\SaidaEngine.exe'
$sourceScene = Join-Path $demoRoot 'scenes\main.scene'
$document = Get-Content -LiteralPath $sourceScene -Raw | ConvertFrom-Json
if ($Mode -eq 'NoAO' -or $Mode -eq 'NoAOOrShadows') {
    $document.scene.settings.aoEnabled = $false
}
function Disable-Shadows($node) {
    if ($node.type -eq 'LightNode') {
        $node | Add-Member -NotePropertyName castShadows -NotePropertyValue $false -Force
    }
    foreach ($child in $node.children) { Disable-Shadows $child }
}
if ($Mode -eq 'NoShadows' -or $Mode -eq 'NoAOOrShadows') {
    Disable-Shadows $document.scene
}
$outputDir = Join-Path $demoRoot 'build\lighting-diagnostics'
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
$outputScene = Join-Path $outputDir ($Mode + '.scene')
$encoding = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($outputScene, ($document | ConvertTo-Json -Depth 100), $encoding)
Write-Host "Lighting diagnostic: $Mode"
if ($PrepareOnly) {
    Write-Output $outputScene
    exit 0
}
if (-not (Test-Path -LiteralPath $engineExe -PathType Leaf)) {
    throw "Engine executable missing: $engineExe"
}
if (Test-Path -LiteralPath 'C:\msys64\ucrt64\bin') {
    $env:PATH = 'C:\msys64\ucrt64\bin;C:\msys64\usr\bin;' + $env:PATH
}
& $engineExe --project (Join-Path $demoRoot 'VRDemo.saidaproj') --xr --scene $outputScene
exit $LASTEXITCODE
