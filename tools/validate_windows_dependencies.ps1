[CmdletBinding()]
param(
    [string]$BundleDir = 'build/bin',
    [string[]]$EntryPoints = @(
        'SaidaEngine.exe',
        'SaidaEngineRuntime.exe',
        'SaidaEngineHub.exe',
        'saida_tool.exe'
    ),
    [string]$OutputPath = 'build/release/windows-dependencies.json'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$bundle = if ([System.IO.Path]::IsPathRooted($BundleDir)) {
    [System.IO.Path]::GetFullPath($BundleDir)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $root $BundleDir))
}
$output = if ([System.IO.Path]::IsPathRooted($OutputPath)) {
    [System.IO.Path]::GetFullPath($OutputPath)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $root $OutputPath))
}
$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $root 'build')).TrimEnd('\', '/')
if (-not $output.StartsWith($buildRoot + [System.IO.Path]::DirectorySeparatorChar,
                            [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "OutputPath must stay under $buildRoot"
}
if (-not (Test-Path -LiteralPath $bundle -PathType Container)) {
    throw "Missing Windows bundle: $bundle"
}

$systemDlls = @(
    'advapi32.dll', 'bcrypt.dll', 'cabinet.dll', 'cfgmgr32.dll',
    'comctl32.dll', 'comdlg32.dll', 'crypt32.dll', 'd3d11.dll',
    'd3d12.dll', 'dbghelp.dll', 'dwmapi.dll', 'dxgi.dll', 'gdi32.dll',
    'imm32.dll', 'iphlpapi.dll', 'kernel32.dll', 'msvcrt.dll',
    'ntdll.dll', 'ole32.dll', 'oleaut32.dll', 'powrprof.dll',
    'propsys.dll', 'rpcrt4.dll', 'setupapi.dll', 'shell32.dll',
    'shlwapi.dll', 'user32.dll', 'ucrtbase.dll', 'version.dll',
    'vulkan-1.dll', 'winmm.dll', 'wintrust.dll', 'ws2_32.dll'
)
$forbiddenDynamicRuntimes = @(
    'libgcc_s_seh-1.dll',
    'libstdc++-6.dll',
    'libwinpthread-1.dll'
)

function Is-System-Dll([string]$Name) {
    $lower = $Name.ToLowerInvariant()
    $lower -like 'api-ms-win-*' -or
        $lower -like 'ext-ms-win-*' -or
        $systemDlls -contains $lower
}

function Relative-Path([string]$Path) {
    $full = [System.IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith(
            $bundle.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "PE file escapes bundle: $full"
    }
    $full.Substring($bundle.TrimEnd('\', '/').Length + 1).Replace('\', '/')
}

function Assert-ReadableRange(
    [System.IO.BinaryReader]$Reader,
    [long]$Offset,
    [long]$Bytes,
    [string]$Label
) {
    if ($Offset -lt 0 -or $Bytes -lt 0 -or
        $Offset -gt $Reader.BaseStream.Length - $Bytes) {
        throw "Malformed PE ($Label outside file)"
    }
}

function Read-U16(
    [System.IO.BinaryReader]$Reader,
    [long]$Offset,
    [string]$Label
) {
    Assert-ReadableRange $Reader $Offset 2 $Label
    $Reader.BaseStream.Position = $Offset
    $Reader.ReadUInt16()
}

function Read-U32(
    [System.IO.BinaryReader]$Reader,
    [long]$Offset,
    [string]$Label
) {
    Assert-ReadableRange $Reader $Offset 4 $Label
    $Reader.BaseStream.Position = $Offset
    $Reader.ReadUInt32()
}

function Convert-RvaToFileOffset(
    [uint32]$Rva,
    [object[]]$Sections,
    [uint32]$SizeOfHeaders,
    [long]$FileLength,
    [string]$Label
) {
    if ($Rva -lt $SizeOfHeaders) {
        if ([long]$Rva -ge $FileLength) {
            throw "Malformed PE ($Label header RVA outside file)"
        }
        return [long]$Rva
    }
    foreach ($section in $Sections) {
        $span = [Math]::Max([uint64]$section.virtualSize, [uint64]$section.rawSize)
        $start = [uint64]$section.virtualAddress
        $value = [uint64]$Rva
        if ($value -ge $start -and $value -lt $start + $span) {
            $delta = $value - $start
            if ($delta -ge [uint64]$section.rawSize) {
                throw "Malformed PE ($Label points into virtual-only section data)"
            }
            $offset = [uint64]$section.rawOffset + $delta
            if ($offset -ge [uint64]$FileLength) {
                throw "Malformed PE ($Label file offset outside file)"
            }
            return [long]$offset
        }
    }
    throw "Malformed PE ($Label RVA 0x$($Rva.ToString('x')) is unmapped)"
}

function Read-ImportName(
    [System.IO.BinaryReader]$Reader,
    [uint32]$Rva,
    [object[]]$Sections,
    [uint32]$SizeOfHeaders,
    [string]$Label
) {
    $offset = Convert-RvaToFileOffset `
        $Rva $Sections $SizeOfHeaders $Reader.BaseStream.Length $Label
    $bytes = New-Object System.Collections.Generic.List[byte]
    for ($i = 0; $i -lt 512; ++$i) {
        Assert-ReadableRange $Reader ($offset + $i) 1 $Label
        $Reader.BaseStream.Position = $offset + $i
        $value = $Reader.ReadByte()
        if ($value -eq 0) {
            if ($bytes.Count -eq 0) { throw "Malformed PE (empty $Label)" }
            $name = [System.Text.Encoding]::ASCII.GetString($bytes.ToArray())
            if ($name.Contains('/') -or $name.Contains('\') -or
                -not $name.EndsWith('.dll', [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "Malformed PE (unsafe import name '$name')"
            }
            return $name
        }
        if ($value -lt 0x20 -or $value -gt 0x7e) {
            throw "Malformed PE (non-ASCII byte in $Label)"
        }
        $bytes.Add($value)
    }
    throw "Malformed PE ($Label exceeds 511 bytes)"
}

function Inspect-Pe([string]$Path) {
    $imports = New-Object 'System.Collections.Generic.HashSet[string]' (
        [System.StringComparer]::OrdinalIgnoreCase)
    $stream = [System.IO.File]::Open(
        $Path, [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
    try {
        $reader = New-Object System.IO.BinaryReader($stream)
        try {
            if ((Read-U16 $reader 0 'DOS signature') -ne 0x5a4d) {
                throw "Expected an MZ executable"
            }
            $peOffset = [long](Read-U32 $reader 0x3c 'PE header offset')
            Assert-ReadableRange $reader $peOffset 24 'PE header'
            $reader.BaseStream.Position = $peOffset
            if ($reader.ReadUInt32() -ne 0x00004550) {
                throw "Expected a PE signature"
            }
            if ($reader.ReadUInt16() -ne 0x8664) {
                throw "Expected an x86_64 PE machine"
            }
            $sectionCount = $reader.ReadUInt16()
            if ($sectionCount -eq 0 -or $sectionCount -gt 96) {
                throw "Malformed PE (invalid section count $sectionCount)"
            }
            $optionalSize = Read-U16 $reader ($peOffset + 20) 'optional header size'
            $optionalOffset = $peOffset + 24
            if ($optionalSize -lt 112) {
                throw "Malformed PE (optional header is too small)"
            }
            Assert-ReadableRange $reader $optionalOffset $optionalSize 'optional header'
            if ((Read-U16 $reader $optionalOffset 'optional header magic') -ne 0x20b) {
                throw "Expected a PE32+ optional header"
            }
            $sizeOfHeaders =
                Read-U32 $reader ($optionalOffset + 60) 'SizeOfHeaders'
            $directoryCount =
                Read-U32 $reader ($optionalOffset + 108) 'NumberOfRvaAndSizes'
            $sectionTable = $optionalOffset + $optionalSize
            Assert-ReadableRange $reader $sectionTable ([long]$sectionCount * 40) 'section table'
            $sections = @()
            for ($i = 0; $i -lt $sectionCount; ++$i) {
                $sectionOffset = $sectionTable + [long]$i * 40
                $sections += [pscustomobject]@{
                    virtualSize = Read-U32 $reader ($sectionOffset + 8) 'section VirtualSize'
                    virtualAddress = Read-U32 $reader ($sectionOffset + 12) 'section VirtualAddress'
                    rawSize = Read-U32 $reader ($sectionOffset + 16) 'section SizeOfRawData'
                    rawOffset = Read-U32 $reader ($sectionOffset + 20) 'section PointerToRawData'
                }
            }

            # IMAGE_DIRECTORY_ENTRY_IMPORT (index 1), descriptors of 20 bytes.
            if ($directoryCount -gt 1) {
                $importRva = Read-U32 $reader ($optionalOffset + 120) 'import directory RVA'
                $importSize = Read-U32 $reader ($optionalOffset + 124) 'import directory size'
                if ($importRva -ne 0 -or $importSize -ne 0) {
                    if ($importRva -eq 0 -or $importSize -lt 20) {
                        throw "Malformed PE (invalid import directory)"
                    }
                    $terminated = $false
                    $limit = [Math]::Min([int]($importSize / 20 + 1), 4096)
                    for ($i = 0; $i -lt $limit; ++$i) {
                        $descriptorRva = [uint32]([uint64]$importRva + [uint64]$i * 20)
                        $descriptor = Convert-RvaToFileOffset `
                            $descriptorRva $sections $sizeOfHeaders $reader.BaseStream.Length `
                            "import descriptor $i"
                        $originalThunk = Read-U32 $reader $descriptor 'OriginalFirstThunk'
                        $timeDate = Read-U32 $reader ($descriptor + 4) 'Import TimeDateStamp'
                        $forwarder = Read-U32 $reader ($descriptor + 8) 'ForwarderChain'
                        $nameRva = Read-U32 $reader ($descriptor + 12) 'import Name RVA'
                        $firstThunk = Read-U32 $reader ($descriptor + 16) 'FirstThunk'
                        if (($originalThunk -bor $timeDate -bor $forwarder -bor
                             $nameRva -bor $firstThunk) -eq 0) {
                            $terminated = $true
                            break
                        }
                        if ($nameRva -eq 0) {
                            throw "Malformed PE (import descriptor $i has no DLL name)"
                        }
                        $name = Read-ImportName `
                            $reader $nameRva $sections $sizeOfHeaders "import DLL name $i"
                        [void]$imports.Add($name)
                    }
                    if (-not $terminated) {
                        throw "Malformed PE (unterminated import descriptor table)"
                    }
                }
            }

            # IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT (index 13), ImgDelayDescr
            # records of eight DWORDs. Modern PE32+ images store RVAs (grAttrs
            # bit 0); the obsolete VA form is rejected instead of guessed.
            if ($directoryCount -gt 13 -and $optionalSize -ge 224) {
                $delayRva = Read-U32 $reader ($optionalOffset + 216) 'delay import RVA'
                $delaySize = Read-U32 $reader ($optionalOffset + 220) 'delay import size'
                if ($delayRva -ne 0 -or $delaySize -ne 0) {
                    if ($delayRva -eq 0 -or $delaySize -lt 32) {
                        throw "Malformed PE (invalid delay import directory)"
                    }
                    $terminated = $false
                    $limit = [Math]::Min([int]($delaySize / 32 + 1), 4096)
                    for ($i = 0; $i -lt $limit; ++$i) {
                        $descriptorRva = [uint32]([uint64]$delayRva + [uint64]$i * 32)
                        $descriptor = Convert-RvaToFileOffset `
                            $descriptorRva $sections $sizeOfHeaders $reader.BaseStream.Length `
                            "delay import descriptor $i"
                        $values = @()
                        for ($field = 0; $field -lt 8; ++$field) {
                            $values += Read-U32 $reader ($descriptor + $field * 4) `
                                "delay import descriptor $i field $field"
                        }
                        $nonZero = $false
                        foreach ($value in $values) {
                            if ($value -ne 0) { $nonZero = $true; break }
                        }
                        if (-not $nonZero) {
                            $terminated = $true
                            break
                        }
                        if (($values[0] -band 1) -eq 0) {
                            throw "Malformed PE (obsolete VA-form delay import descriptor)"
                        }
                        if ($values[1] -eq 0) {
                            throw "Malformed PE (delay import descriptor $i has no DLL name)"
                        }
                        $name = Read-ImportName `
                            $reader ([uint32]$values[1]) $sections $sizeOfHeaders `
                            "delay import DLL name $i"
                        [void]$imports.Add($name)
                    }
                    if (-not $terminated) {
                        throw "Malformed PE (unterminated delay import descriptor table)"
                    }
                }
            }
        } finally {
            $reader.Dispose()
        }
    } catch {
        throw "Expected a valid Windows x64 PE file ($(Relative-Path $Path)): $($_.Exception.Message)"
    } finally {
        $stream.Dispose()
    }
    $sortedImports = [string[]]@($imports)
    [Array]::Sort($sortedImports, [System.StringComparer]::OrdinalIgnoreCase)
    [ordered]@{
        path = (Relative-Path $Path)
        bytes = (Get-Item -LiteralPath $Path).Length
        sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
        imports = $sortedImports
    }
}

$dllIndex = @{}
foreach ($dll in Get-ChildItem -LiteralPath $bundle -Recurse -File -Filter '*.dll') {
    $key = $dll.Name.ToLowerInvariant()
    if ($dllIndex.ContainsKey($key)) {
        throw "Ambiguous bundled DLL name: $($dll.Name)"
    }
    $dllIndex[$key] = $dll.FullName
}

$pending = New-Object System.Collections.Generic.Queue[string]
foreach ($entryPoint in $EntryPoints) {
    if ([System.IO.Path]::IsPathRooted($entryPoint) -or $entryPoint.Contains('..')) {
        throw "Unsafe entry point: $entryPoint"
    }
    $path = [System.IO.Path]::GetFullPath((Join-Path $bundle $entryPoint))
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing Windows entry point: $entryPoint"
    }
    $pending.Enqueue($path)
}

$visited = @{}
$records = New-Object System.Collections.Generic.List[object]
while ($pending.Count -gt 0) {
    $path = $pending.Dequeue()
    $key = $path.ToLowerInvariant()
    if ($visited.ContainsKey($key)) { continue }
    $visited[$key] = $true

    $record = Inspect-Pe $path
    $classified = New-Object System.Collections.Generic.List[object]
    foreach ($import in @($record.imports)) {
        $lower = $import.ToLowerInvariant()
        if ($forbiddenDynamicRuntimes -contains $lower) {
            throw "Forbidden dynamic MinGW runtime imported by $($record.path): $import"
        }
        if (Is-System-Dll $import) {
            $classification = 'system'
        } elseif ($dllIndex.ContainsKey($lower)) {
            $classification = 'bundled'
            $pending.Enqueue([string]$dllIndex[$lower])
        } else {
            throw "Missing non-system DLL imported by $($record.path): $import"
        }
        $classified.Add([ordered]@{
            name = $import
            classification = $classification
        })
    }
    $record.imports = $classified.ToArray()
    $records.Add($record)
}

$commit = (& git -C $root rev-parse HEAD).Trim()
$commitTime = (& git -C $root show -s --format=%cI HEAD).Trim()
$report = [ordered]@{
    schema = 1
    engineCommit = $commit
    generatedAtUtc = ([DateTimeOffset]::Parse($commitTime).UtcDateTime.ToString('o'))
    architecture = 'x86_64'
    bundle = [System.IO.Path]::GetFileName($bundle.TrimEnd('\', '/'))
    entryPoints = @($EntryPoints | ForEach-Object { $_.Replace('\', '/') })
    files = @($records.ToArray() | Sort-Object path)
}

$parent = Split-Path -Parent $output
New-Item -ItemType Directory -Force -Path $parent | Out-Null
$encoding = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText(
    $output,
    (($report | ConvertTo-Json -Depth 10).TrimEnd() + "`r`n"),
    $encoding)

Write-Host "WINDOWS DEPENDENCY VERIFY PASS"
Write-Host "  entry points: $($EntryPoints.Count)"
Write-Host "  PE files in closure: $($records.Count)"
Write-Host "  report: $output"
