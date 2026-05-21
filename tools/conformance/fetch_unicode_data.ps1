#!/usr/bin/env pwsh

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [Alias("OutputDir")]
    [string] $OutputDirectory,

    [switch] $Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$unicode_version = "16.0.0"
$emoji_version = "16.0"
$manifest_file_name = "vnm_terminal_unicode_data_manifest.json"

$source_files = @(
    [pscustomobject]@{
        RelativePath = "EastAsianWidth.txt"
        Url          = "https://www.unicode.org/Public/16.0.0/ucd/EastAsianWidth.txt"
    },
    [pscustomobject]@{
        RelativePath = "UnicodeData.txt"
        Url          = "https://www.unicode.org/Public/16.0.0/ucd/UnicodeData.txt"
    },
    [pscustomobject]@{
        RelativePath = "PropList.txt"
        Url          = "https://www.unicode.org/Public/16.0.0/ucd/PropList.txt"
    },
    [pscustomobject]@{
        RelativePath = "auxiliary/GraphemeBreakTest.txt"
        Url          = "https://www.unicode.org/Public/16.0.0/ucd/auxiliary/GraphemeBreakTest.txt"
    },
    [pscustomobject]@{
        RelativePath = "emoji/emoji-data.txt"
        Url          = "https://www.unicode.org/Public/16.0.0/ucd/emoji/emoji-data.txt"
    },
    [pscustomobject]@{
        RelativePath = "emoji/emoji-variation-sequences.txt"
        Url          = "https://www.unicode.org/Public/16.0.0/ucd/emoji/emoji-variation-sequences.txt"
    },
    [pscustomobject]@{
        RelativePath = "emoji/emoji-zwj-sequences.txt"
        Url          = "https://www.unicode.org/Public/emoji/16.0/emoji-zwj-sequences.txt"
    }
)

function Get-RepositoryRoot
{
    return [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../.."))
}

function Resolve-OutputPath
{
    param(
        [string] $Path
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return [System.IO.Path]::GetFullPath(
            (Join-Path (Get-RepositoryRoot) "build/unicode-$unicode_version"))
    }

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }

    return [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Path))
}

function Ensure-Directory
{
    param(
        [string] $Path
    )

    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        throw "path exists but is not a directory: $Path"
    }

    New-Item -ItemType Directory -Force -Path $Path | Out-Null
}

function Get-ManifestPath
{
    param(
        [string] $OutputPath
    )

    return Join-Path $OutputPath $manifest_file_name
}

function Get-SourceFilePath
{
    param(
        [object] $File,
        [string] $OutputPath
    )

    return Join-Path $OutputPath $File.RelativePath
}

function Test-ExistingUnicodeDataArtifact
{
    param(
        [string] $OutputPath
    )

    if (Test-Path -LiteralPath (Get-ManifestPath $OutputPath)) {
        return $true
    }

    foreach ($file in $source_files) {
        if (Test-Path -LiteralPath (Get-SourceFilePath -File $file -OutputPath $OutputPath)) {
            return $true
        }
    }

    return $false
}

function Save-DownloadedFile
{
    param(
        [string] $Uri,
        [string] $Destination
    )

    $destination_dir = Split-Path -Parent $Destination
    Ensure-Directory $destination_dir

    if (Test-Path -LiteralPath $Destination) {
        if (-not (Test-Path -LiteralPath $Destination -PathType Leaf)) {
            throw "download destination exists but is not a file: $Destination"
        }

        if (-not $Force) {
            throw "download destination already exists; rerun with -Force: $Destination"
        }
    }

    $destination_name = [System.IO.Path]::GetFileName($Destination)
    $temp_name        = ".$destination_name.$([guid]::NewGuid().ToString("N")).tmp"
    $temp_path        = Join-Path $destination_dir $temp_name

    try {
        Write-Host "download: $Uri"
        Invoke-WebRequest -Uri $Uri -OutFile $temp_path -UseBasicParsing -TimeoutSec 90

        $downloaded_file = Get-Item -LiteralPath $temp_path
        if ($downloaded_file.Length -le 0) {
            throw "download produced an empty file"
        }

        if ($Force) {
            Move-Item -LiteralPath $temp_path -Destination $Destination -Force
        }
        else {
            Move-Item -LiteralPath $temp_path -Destination $Destination
        }
    }
    catch {
        if (Test-Path -LiteralPath $temp_path) {
            Remove-Item -LiteralPath $temp_path -Force
        }
        throw "failed to download $Uri to $Destination`: $($_.Exception.Message)"
    }
}

function Get-RequiredProperty
{
    param(
        [object] $Object,
        [string] $Property,
        [string] $Label
    )

    $property_info = $Object.PSObject.Properties[$Property]
    if ($null -eq $property_info) {
        throw "$Label is missing property $Property"
    }

    return $property_info.Value
}

function Get-RequiredStringProperty
{
    param(
        [object] $Object,
        [string] $Property,
        [string] $Expected,
        [string] $Label
    )

    $value = Get-RequiredProperty -Object $Object -Property $Property -Label $Label
    if (-not ($value -is [string])) {
        throw "$Label property $Property is not a string"
    }

    if ($value -ne $Expected) {
        throw "$Label property $Property is '$value', expected '$Expected'"
    }

    return $value
}

function Get-RequiredInt64Property
{
    param(
        [object] $Object,
        [string] $Property,
        [Int64] $Expected,
        [string] $Label
    )

    $value = Get-RequiredProperty -Object $Object -Property $Property -Label $Label
    if (-not ($value -is [int]) -and -not ($value -is [long])) {
        throw "$Label property $Property is not an integer"
    }

    if ([Int64] $value -ne $Expected) {
        throw "$Label property $Property is $value, expected $Expected"
    }

    return [Int64] $value
}

function Get-ManifestFileEntry
{
    param(
        [object] $File,
        [string] $OutputPath
    )

    $path = Get-SourceFilePath -File $File -OutputPath $OutputPath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "manifest source file is missing: $path"
    }

    $file_info = Get-Item -LiteralPath $path
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()

    return [ordered]@{
        relative_path = $File.RelativePath
        url           = $File.Url
        sha256        = $hash
        bytes         = $file_info.Length
    }
}

function Write-DataManifest
{
    param(
        [string] $OutputPath
    )

    $files = @()
    foreach ($file in $source_files) {
        $files += Get-ManifestFileEntry -File $file -OutputPath $OutputPath
    }

    $manifest = [ordered]@{
        artifact_kind  = "vnm_terminal_unicode_conformance_data"
        generated_by   = "tools/conformance/fetch_unicode_data.ps1"
        unicode_version = $unicode_version
        emoji_version   = $emoji_version
        files           = $files
    }

    $manifest_path = Get-ManifestPath $OutputPath
    $manifest_name = [System.IO.Path]::GetFileName($manifest_path)
    $temp_name     = ".$manifest_name.$([guid]::NewGuid().ToString("N")).tmp"
    $temp_path     = Join-Path $OutputPath $temp_name

    try {
        $json = ($manifest | ConvertTo-Json -Depth 5) + "`n"
        $utf8_no_bom = [System.Text.UTF8Encoding]::new($false)
        [System.IO.File]::WriteAllText($temp_path, $json, $utf8_no_bom)
        Move-Item -LiteralPath $temp_path -Destination $manifest_path -Force
        Write-Host "manifest: $manifest_path"
    }
    catch {
        if (Test-Path -LiteralPath $temp_path) {
            Remove-Item -LiteralPath $temp_path -Force
        }
        throw "failed to write Unicode data manifest $manifest_path`: $($_.Exception.Message)"
    }
}

function Read-DataManifest
{
    param(
        [string] $OutputPath
    )

    $manifest_path = Get-ManifestPath $OutputPath
    if (-not (Test-Path -LiteralPath $manifest_path -PathType Leaf)) {
        throw "Unicode data manifest is missing: $manifest_path"
    }

    try {
        return Get-Content -LiteralPath $manifest_path -Raw | ConvertFrom-Json
    }
    catch {
        throw "Unicode data manifest is not valid JSON: $manifest_path`: $($_.Exception.Message)"
    }
}

function Assert-ExistingDataManifest
{
    param(
        [string] $OutputPath
    )

    try {
        $manifest = Read-DataManifest $OutputPath

        Get-RequiredStringProperty `
            -Object $manifest `
            -Property "artifact_kind" `
            -Expected "vnm_terminal_unicode_conformance_data" `
            -Label "manifest" | Out-Null
        Get-RequiredStringProperty `
            -Object $manifest `
            -Property "unicode_version" `
            -Expected $unicode_version `
            -Label "manifest" | Out-Null
        Get-RequiredStringProperty `
            -Object $manifest `
            -Property "emoji_version" `
            -Expected $emoji_version `
            -Label "manifest" | Out-Null

        $manifest_files = @(Get-RequiredProperty -Object $manifest -Property "files" -Label "manifest")
        if ($manifest_files.Count -ne $source_files.Count) {
            throw "manifest records $($manifest_files.Count) files, expected $($source_files.Count)"
        }

        $files_by_path = @{}
        foreach ($file_entry in $manifest_files) {
            $relative_path = Get-RequiredProperty `
                -Object $file_entry `
                -Property "relative_path" `
                -Label "manifest file entry"
            if (-not ($relative_path -is [string]) -or [string]::IsNullOrWhiteSpace($relative_path)) {
                throw "manifest file entry relative_path is not a non-empty string"
            }
            if ($files_by_path.ContainsKey($relative_path)) {
                throw "manifest contains duplicate file entry: $relative_path"
            }
            $files_by_path[$relative_path] = $file_entry
        }

        foreach ($file in $source_files) {
            if (-not $files_by_path.ContainsKey($file.RelativePath)) {
                throw "manifest does not include $($file.RelativePath)"
            }

            $file_entry = $files_by_path[$file.RelativePath]
            Get-RequiredStringProperty `
                -Object $file_entry `
                -Property "url" `
                -Expected $file.Url `
                -Label "manifest entry $($file.RelativePath)" | Out-Null

            $path = Get-SourceFilePath -File $file -OutputPath $OutputPath
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
                throw "manifested Unicode data file is missing: $path"
            }

            $file_info     = Get-Item -LiteralPath $path
            $expected_hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()

            Get-RequiredStringProperty `
                -Object $file_entry `
                -Property "sha256" `
                -Expected $expected_hash `
                -Label "manifest entry $($file.RelativePath)" | Out-Null
            Get-RequiredInt64Property `
                -Object $file_entry `
                -Property "bytes" `
                -Expected $file_info.Length `
                -Label "manifest entry $($file.RelativePath)" | Out-Null
        }
    }
    catch {
        throw "existing Unicode data is not backed by a valid manifest: $($_.Exception.Message). Rerun with -Force to redownload and write a fresh manifest."
    }
}

try {
    $output_path = Resolve-OutputPath $OutputDirectory

    Ensure-Directory $output_path
    Ensure-Directory (Join-Path $output_path "auxiliary")
    Ensure-Directory (Join-Path $output_path "emoji")

    Write-Host "Unicode data target: $output_path"
    Write-Host "Unicode version: $unicode_version; emoji version: $emoji_version"

    if ((Test-ExistingUnicodeDataArtifact $output_path) -and -not $Force) {
        Assert-ExistingDataManifest $output_path
        Write-Host "existing Unicode data manifest validated: $(Get-ManifestPath $output_path)"
    }
    else {
        foreach ($file in $source_files) {
            $destination = Get-SourceFilePath -File $file -OutputPath $output_path
            Save-DownloadedFile -Uri $file.Url -Destination $destination
        }
        Write-DataManifest $output_path
    }

    Write-Host "Unicode data fetch complete: $output_path"
}
catch {
    [Console]::Error.WriteLine("ERROR: $($_.Exception.Message)")
    exit 1
}
