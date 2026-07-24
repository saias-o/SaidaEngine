[CmdletBinding()]
param(
    [string]$OutputDir = 'build/release/compliance',
    [switch]$AllowDirty
)

# Generates the release compliance set from two reviewed, fail-closed inputs:
# compliance/components.json and compliance/assets.json. A new third_party
# directory or a new tracked asset extension fails until it has a license and
# provenance decision. Output is SPDX 2.3 JSON plus human-readable notices and
# a hash-pinned asset/model inventory.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $root 'build')).TrimEnd('\', '/')
. (Join-Path $PSScriptRoot 'git_worktree_state.ps1')
$out = if ([System.IO.Path]::IsPathRooted($OutputDir)) {
    [System.IO.Path]::GetFullPath($OutputDir)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $root $OutputDir))
}
if (-not $out.StartsWith($buildRoot + [System.IO.Path]::DirectorySeparatorChar,
                         [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "OutputDir must stay under $buildRoot"
}

function Resolve-RepoPath([string]$Path) {
    [System.IO.Path]::GetFullPath((Join-Path $root $Path))
}

function Write-Utf8NoBom([string]$Path, [string]$Text) {
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, ($Text.TrimEnd() + "`r`n"), $encoding)
}

function Json-Text($Value, [int]$Depth = 20) {
    $Value | ConvertTo-Json -Depth $Depth
}

function Sha256([string]$Path) {
    (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Safe-SpdxId([string]$Prefix, [string]$Value) {
    $safe = [regex]::Replace($Value, '[^A-Za-z0-9.-]', '-')
    "SPDXRef-$Prefix-$safe"
}

Push-Location $root
try {
    $worktree = Get-GitWorktreeState
    $status = $worktree.Status
    $dirty = $worktree.Dirty
    if ($dirty -and -not $AllowDirty) {
        throw "Compliance output requires a clean Git worktree: $status (use -AllowDirty only for development proofs)"
    }

    $componentConfigPath = Resolve-RepoPath 'compliance/components.json'
    $assetConfigPath = Resolve-RepoPath 'compliance/assets.json'
    $componentConfig = Get-Content -Raw -Encoding UTF8 -LiteralPath $componentConfigPath |
        ConvertFrom-Json
    $assetConfig = Get-Content -Raw -Encoding UTF8 -LiteralPath $assetConfigPath |
        ConvertFrom-Json
    if ($componentConfig.schema -ne 1 -or $assetConfig.schema -ne 1) {
        throw "Unsupported compliance input schema"
    }

    $components = @($componentConfig.components)
    $componentIds = @($components | ForEach-Object { [string]$_.id })
    if (($componentIds | Sort-Object -Unique).Count -ne $componentIds.Count) {
        throw "Duplicate component id in compliance/components.json"
    }

    # Every vendored root is reviewed, and every top-level third_party
    # directory is represented exactly once.
    $actualVendored = @(Get-ChildItem -LiteralPath (Resolve-RepoPath 'third_party') -Directory |
        ForEach-Object { "third_party/$($_.Name)" } | Sort-Object)
    $declaredVendored = @($components |
        Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_.vendoredRoot) } |
        ForEach-Object { ([string]$_.vendoredRoot).Replace('\', '/') } | Sort-Object)
    $missingVendored = @($actualVendored | Where-Object { $_ -notin $declaredVendored })
    $staleVendored = @($declaredVendored | Where-Object { $_ -notin $actualVendored })
    if ($missingVendored.Count -or $staleVendored.Count) {
        throw "Vendored component coverage mismatch; missing=[$($missingVendored -join ', ')], stale=[$($staleVendored -join ', ')]"
    }

    foreach ($component in $components) {
        if ([string]::IsNullOrWhiteSpace([string]$component.licenseDeclared) -or
            [string]::IsNullOrWhiteSpace([string]$component.licenseConcluded) -or
            [string]::IsNullOrWhiteSpace([string]$component.copyrightText)) {
            throw "Incomplete license fields for component $($component.id)"
        }
        foreach ($licenseFile in @($component.licenseFiles)) {
            $licensePath = Resolve-RepoPath ([string]$licenseFile)
            if (-not (Test-Path -LiteralPath $licensePath -PathType Leaf)) {
                throw "Missing license evidence for $($component.id): $licenseFile"
            }
        }
    }

    # Tracked media/animation assets are also fail-closed: additions require an
    # explicit row, including a distribution decision for unresolved data.
    $extensions = @($assetConfig.trackedExtensions | ForEach-Object {
        ([string]$_).ToLowerInvariant()
    })
    $trackedAssets = @(& git ls-files | Where-Object {
        $extensions -contains [System.IO.Path]::GetExtension($_).ToLowerInvariant()
    } | ForEach-Object { $_.Replace('\', '/') } | Sort-Object)
    $assetRows = @($assetConfig.assets)
    $declaredAssets = @($assetRows | ForEach-Object {
        ([string]$_.path).Replace('\', '/')
    } | Sort-Object)
    if (($declaredAssets | Sort-Object -Unique).Count -ne $declaredAssets.Count) {
        throw "Duplicate asset path in compliance/assets.json"
    }
    $missingAssets = @($trackedAssets | Where-Object { $_ -notin $declaredAssets })
    $staleAssets = @($declaredAssets | Where-Object { $_ -notin $trackedAssets })
    if ($missingAssets.Count -or $staleAssets.Count) {
        throw "Asset coverage mismatch; missing=[$($missingAssets -join ', ')], stale=[$($staleAssets -join ', ')]"
    }

    $assetRecords = New-Object System.Collections.Generic.List[object]
    foreach ($asset in ($assetRows | Sort-Object path)) {
        $relative = ([string]$asset.path).Replace('\', '/')
        $full = Resolve-RepoPath $relative
        if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
            throw "Missing asset: $relative"
        }
        if ([bool]$asset.distribution -and [string]$asset.license -eq 'NOASSERTION') {
            throw "Distributed asset has unresolved license: $relative"
        }
        $record = [ordered]@{
            path = $relative
            kind = [string]$asset.kind
            distribution = [bool]$asset.distribution
            bytes = (Get-Item -LiteralPath $full).Length
            sha256 = Sha256 $full
            license = [string]$asset.license
            copyrightText = [string]$asset.copyrightText
            provenance = [string]$asset.provenance
        }
        if ($asset.PSObject.Properties.Name -contains 'source') {
            $record['source'] = [string]$asset.source
        }
        if ($asset.PSObject.Properties.Name -contains 'note') {
            $record['note'] = [string]$asset.note
        }
        $assetRecords.Add($record)
    }

    $commit = (& git rev-parse HEAD).Trim()
    $commitTime = (& git show -s --format=%cI HEAD).Trim()
    $documentSuffix = if ($dirty) { "$commit-dirty" } else { $commit }
    $rootPackageId = 'SPDXRef-Package-SaidaEngine'

    $packages = New-Object System.Collections.Generic.List[object]
    $packages.Add([ordered]@{
        name = 'Saida Engine'
        SPDXID = $rootPackageId
        versionInfo = $commit
        downloadLocation = "git+https://github.com/saias-o/SaidaEngine.git@$commit"
        filesAnalyzed = $false
        licenseConcluded = 'GPL-3.0-only'
        licenseDeclared = 'GPL-3.0-only'
        copyrightText = 'Copyright Saida Engine contributors'
    })

    $relationships = New-Object System.Collections.Generic.List[object]
    $relationships.Add([ordered]@{
        spdxElementId = 'SPDXRef-DOCUMENT'
        relationshipType = 'DESCRIBES'
        relatedSpdxElement = $rootPackageId
    })

    $componentRecords = New-Object System.Collections.Generic.List[object]
    foreach ($component in ($components | Sort-Object id)) {
        $id = Safe-SpdxId 'Package' ([string]$component.id)
        $revision = ''
        if (-not [string]::IsNullOrWhiteSpace([string]$component.vendoredRoot)) {
            $revision = (& git rev-parse "HEAD:$($component.vendoredRoot)").Trim()
        }
        $version = [string]$component.version
        if ([string]::IsNullOrWhiteSpace($version) -and $revision) {
            $version = "git-$($revision.Substring(0, 12))"
        }
        $package = [ordered]@{
            name = [string]$component.name
            SPDXID = $id
            downloadLocation = [string]$component.downloadLocation
            filesAnalyzed = $false
            licenseConcluded = [string]$component.licenseConcluded
            licenseDeclared = [string]$component.licenseDeclared
            copyrightText = [string]$component.copyrightText
        }
        if (-not [string]::IsNullOrWhiteSpace($version)) {
            $package['versionInfo'] = $version
        }
        if ($revision) {
            $package['sourceInfo'] = "Vendored source revision/tree: $revision"
        }
        $packages.Add($package)
        $relationships.Add([ordered]@{
            spdxElementId = $rootPackageId
            relationshipType = 'DEPENDS_ON'
            relatedSpdxElement = $id
        })
        $componentRecords.Add([ordered]@{
            id = [string]$component.id
            name = [string]$component.name
            version = $version
            revision = $revision
            vendoredRoot = [string]$component.vendoredRoot
            downloadLocation = [string]$component.downloadLocation
            licenseDeclared = [string]$component.licenseDeclared
            licenseConcluded = [string]$component.licenseConcluded
            copyrightText = [string]$component.copyrightText
            licenseFiles = @($component.licenseFiles)
        })
    }

    $spdxFiles = New-Object System.Collections.Generic.List[object]
    foreach ($asset in $assetRecords) {
        $id = Safe-SpdxId 'File' ([string]$asset.path)
        $spdxFiles.Add([ordered]@{
            fileName = "./$($asset.path)"
            SPDXID = $id
            checksums = @([ordered]@{
                algorithm = 'SHA256'
                checksumValue = $asset.sha256
            })
            licenseConcluded = $asset.license
            licenseInfoInFiles = @($asset.license)
            copyrightText = $asset.copyrightText
            comment = "distribution=$($asset.distribution); provenance=$($asset.provenance)"
        })
        $relationships.Add([ordered]@{
            spdxElementId = $rootPackageId
            relationshipType = 'CONTAINS'
            relatedSpdxElement = $id
        })
    }

    $sbom = [ordered]@{
        spdxVersion = 'SPDX-2.3'
        dataLicense = 'CC0-1.0'
        SPDXID = 'SPDXRef-DOCUMENT'
        name = "Saida-Engine-$commit"
        documentNamespace = "https://saida.engine/spdx/$documentSuffix"
        creationInfo = [ordered]@{
            created = ([DateTimeOffset]::Parse($commitTime).UtcDateTime.ToString('yyyy-MM-ddTHH:mm:ssZ'))
            creators = @('Tool: tools/generate_release_compliance.ps1')
        }
        documentDescribes = @($rootPackageId)
        packages = $packages.ToArray()
        files = $spdxFiles.ToArray()
        relationships = $relationships.ToArray()
        annotations = @([ordered]@{
            annotationDate = ([DateTimeOffset]::Parse($commitTime).UtcDateTime.ToString('yyyy-MM-ddTHH:mm:ssZ'))
            annotationType = 'OTHER'
            annotator = 'Tool: tools/generate_release_compliance.ps1'
            comment = "dirty=$($dirty.ToString().ToLowerInvariant()); source inputs are compliance/components.json and compliance/assets.json"
        })
    }

    $assetInventory = [ordered]@{
        schema = 1
        engineCommit = $commit
        dirty = $dirty
        generatedFrom = 'compliance/assets.json'
        assets = $assetRecords.ToArray()
    }

    if (Test-Path -LiteralPath $out) {
        Remove-Item -LiteralPath $out -Recurse -Force
    }
    New-Item -ItemType Directory -Path $out | Out-Null
    $sbomPath = Join-Path $out 'sbom.spdx.json'
    $assetPath = Join-Path $out 'assets-models.json'
    Write-Utf8NoBom $sbomPath (Json-Text $sbom)
    Write-Utf8NoBom $assetPath (Json-Text $assetInventory)

    $notice = New-Object System.Collections.Generic.List[string]
    $notice.Add('SAIDA ENGINE RELEASE NOTICES')
    $notice.Add("Engine commit: $commit")
    $notice.Add("Dirty source: $($dirty.ToString().ToLowerInvariant())")
    $notice.Add('')
    $notice.Add('Saida Engine is distributed under GPL-3.0-only.')
    $notice.Add('The complete project license follows at the end of this file.')
    $notice.Add('')
    $notice.Add('THIRD-PARTY COMPONENTS')
    foreach ($component in ($components | Sort-Object name)) {
        if (-not [bool]$component.noticeRequired) { continue }
        $notice.Add('')
        $notice.Add("[$($component.name)]")
        $notice.Add("SPDX declared: $($component.licenseDeclared)")
        $notice.Add("SPDX concluded: $($component.licenseConcluded)")
        $notice.Add("Copyright: $($component.copyrightText)")
        $notice.Add("Source: $($component.downloadLocation)")
        $notice.Add("License evidence: $(@($component.licenseFiles) -join ', ')")
    }
    $notice.Add('')
    $notice.Add('ASSETS AND MODELS')
    foreach ($asset in $assetRecords) {
        $notice.Add('')
        $notice.Add("[$($asset.path)]")
        $notice.Add("Distributed: $($asset.distribution.ToString().ToLowerInvariant())")
        $notice.Add("SPDX: $($asset.license)")
        $notice.Add("Copyright: $($asset.copyrightText)")
        $notice.Add("Provenance: $($asset.provenance)")
        if ($asset.Contains('source')) { $notice.Add("Source: $($asset.source)") }
        if ($asset.Contains('note')) { $notice.Add("Note: $($asset.note)") }
    }
    $notice.Add('')
    $notice.Add('LICENSE TEXTS')
    $evidenceGroups = @{}
    foreach ($component in $components) {
        if (-not [bool]$component.noticeRequired) { continue }
        foreach ($licenseFile in @($component.licenseFiles)) {
            $key = [string]$licenseFile
            if (-not $evidenceGroups.ContainsKey($key)) {
                $evidenceGroups[$key] = New-Object System.Collections.Generic.List[string]
            }
            $evidenceGroups[$key].Add([string]$component.name)
        }
    }
    foreach ($licenseFile in ($evidenceGroups.Keys | Sort-Object)) {
        $notice.Add('')
        $notice.Add(('=' * 78))
        $notice.Add("Components: $($evidenceGroups[$licenseFile] -join ', ')")
        $notice.Add("Source file: $licenseFile")
        $notice.Add(('-' * 78))
        $text = Get-Content -Raw -Encoding UTF8 -LiteralPath (Resolve-RepoPath $licenseFile)
        $notice.Add($text.Trim())
    }
    $notice.Add('')
    $notice.Add(('=' * 78))
    $notice.Add('Saida Engine GPL-3.0-only license')
    $notice.Add(('-' * 78))
    $notice.Add((Get-Content -Raw -Encoding UTF8 -LiteralPath (Resolve-RepoPath 'LICENSE')).Trim())

    $noticePath = Join-Path $out 'THIRD_PARTY_NOTICES.txt'
    Write-Utf8NoBom $noticePath ($notice -join "`r`n")

    $manifest = [ordered]@{
        schema = 1
        engineCommit = $commit
        dirty = $dirty
        inputs = @(
            [ordered]@{ path = 'compliance/components.json'; sha256 = Sha256 $componentConfigPath },
            [ordered]@{ path = 'compliance/assets.json'; sha256 = Sha256 $assetConfigPath }
        )
        outputs = @(
            [ordered]@{ path = 'sbom.spdx.json'; sha256 = Sha256 $sbomPath },
            [ordered]@{ path = 'assets-models.json'; sha256 = Sha256 $assetPath },
            [ordered]@{ path = 'THIRD_PARTY_NOTICES.txt'; sha256 = Sha256 $noticePath }
        )
        components = $componentRecords.ToArray()
    }
    $manifestPath = Join-Path $out 'compliance-manifest.json'
    Write-Utf8NoBom $manifestPath (Json-Text $manifest)

    Write-Host "RELEASE COMPLIANCE READY: $out"
    Write-Host "  SPDX packages: $($packages.Count)"
    Write-Host "  tracked assets: $($assetRecords.Count)"
    Write-Host "  distributed assets: $(@($assetRecords | Where-Object { $_.distribution }).Count)"
    Write-Host "  excluded unresolved assets: $(@($assetRecords | Where-Object { -not $_.distribution }).Count)"
} finally {
    Pop-Location
}
