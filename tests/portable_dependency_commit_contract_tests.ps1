[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-CommitContract {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Condition,

        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not $Condition) {
        throw "Portable dependency commit contract violation: $Message"
    }
}

function Invoke-CommitProbe {
    param(
        [Parameter(Mandatory = $true)]
        [string]$HelperPath,

        [Parameter(Mandatory = $true)]
        [string]$SourcePath,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedCommit,

        [Parameter(Mandatory = $true)]
        [string]$DependencyName,

        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [byte[]]$GitOutput,

        [int]$GitExitCode = 0
    )

    $outputPath = Join-Path $script:ProbeRoot 'git-output.bin'
    [IO.File]::WriteAllBytes($outputPath, $GitOutput)
    $env:VNM_TEST_GIT_OUTPUT_FILE = $outputPath
    $env:VNM_TEST_GIT_EXIT_CODE = [string]$GitExitCode

    $output = & $HelperPath $SourcePath $ExpectedCommit $DependencyName 2>&1 | Out-String
    return @{
        ExitCode = $LASTEXITCODE
        GitOutput = [Text.ASCIIEncoding]::new().GetString($GitOutput)
        Output = $output
    }
}

$helperPath = Join-Path $SourceRoot 'tools\verify_git_checkout_commit.bat'
$portableScriptPath = Join-Path $SourceRoot 'build_portable.bat'
$script:ProbeRoot = Join-Path ([IO.Path]::GetTempPath()) `
    "vnm-terminal-commit-probe-$([Guid]::NewGuid().ToString('N'))"
$originalPath = $env:PATH
$originalNodeOptions = $env:NODE_OPTIONS
$originalOutputFile = $env:VNM_TEST_GIT_OUTPUT_FILE
$originalExitCode = $env:VNM_TEST_GIT_EXIT_CODE

try {
    Assert-CommitContract (Test-Path -LiteralPath $helperPath -PathType Leaf) `
        'the shared Git commit verifier is missing'

    $portableScript = Get-Content -Raw -LiteralPath $portableScriptPath
    Assert-CommitContract ($portableScript -match
        'verify_git_checkout_commit\.bat" "%FREETYPE_SOURCE_DIR%" "%FREETYPE_COMMIT%" "FreeType"') `
        'FreeType must use the shared exact commit verifier'
    Assert-CommitContract ($portableScript -match
        'verify_git_checkout_commit\.bat" "%MSDFGEN_SOURCE_DIR%" "%MSDFGEN_COMMIT%" "msdfgen"') `
        'msdfgen must use the shared exact commit verifier'
    Assert-CommitContract ($portableScript -notmatch
        'rev-parse HEAD\s*\|\s*findstr') `
        'Git commit validation must not pipe LF-only output through findstr'

    New-Item -ItemType Directory -Path $script:ProbeRoot | Out-Null
    $freeTypePath = Join-Path $script:ProbeRoot 'FreeType source'
    $msdfgenPath = Join-Path $script:ProbeRoot 'msdfgen source'
    New-Item -ItemType Directory -Path $freeTypePath | Out-Null
    New-Item -ItemType Directory -Path $msdfgenPath | Out-Null

    $fakeGitPath = Join-Path $script:ProbeRoot 'fake_git.js'
    $fakeGit = @'
const fs = require("fs");

fs.writeSync(1, fs.readFileSync(process.env.VNM_TEST_GIT_OUTPUT_FILE));
process.exit(Number(process.env.VNM_TEST_GIT_EXIT_CODE || "0"));
'@
    [IO.File]::WriteAllText(
        $fakeGitPath,
        $fakeGit,
        [Text.UTF8Encoding]::new($false))
    $nodePath = (Get-Command node.exe -ErrorAction Stop).Source
    Copy-Item -LiteralPath $nodePath -Destination (Join-Path $script:ProbeRoot 'git.exe')
    $env:PATH = "$script:ProbeRoot;$originalPath"
    $env:NODE_OPTIONS = "--require=$fakeGitPath"

    $freeTypeCommit = '42608f77f20749dd6ddc9e0536788eaad70ea4b5'
    $msdfgenCommit = '6574da1310df433c97ca0fddcab7e463c31e58f8'
    $ascii = [Text.ASCIIEncoding]::new()

    $lfResult = Invoke-CommitProbe $helperPath $freeTypePath $freeTypeCommit `
        'FreeType' ($ascii.GetBytes("$freeTypeCommit`n"))
    Assert-CommitContract ($lfResult.ExitCode -eq 0) `
        "FreeType LF-only exact output was rejected: $($lfResult.Output.Trim())"

    $crlfResult = Invoke-CommitProbe $helperPath $msdfgenPath $msdfgenCommit `
        'msdfgen' ($ascii.GetBytes("$msdfgenCommit`r`n"))
    Assert-CommitContract ($crlfResult.ExitCode -eq 0) `
        "msdfgen CRLF exact output was rejected: $($crlfResult.Output.Trim())"

    $mismatchResult = Invoke-CommitProbe $helperPath $freeTypePath $freeTypeCommit `
        'FreeType' ($ascii.GetBytes("$('0' * 40)`n"))
    Assert-CommitContract ($mismatchResult.ExitCode -ne 0) `
        ("a mismatching commit was accepted with exit $($mismatchResult.ExitCode) for " +
            "'$($mismatchResult.GitOutput.Trim())': $($mismatchResult.Output.Trim())")

    $emptyResult = Invoke-CommitProbe $helperPath $msdfgenPath $msdfgenCommit `
        'msdfgen' ([byte[]]::new(0))
    Assert-CommitContract ($emptyResult.ExitCode -ne 0) `
        "empty Git output was accepted: $($emptyResult.Output.Trim())"

    $multipleResult = Invoke-CommitProbe $helperPath $freeTypePath $freeTypeCommit `
        'FreeType' ($ascii.GetBytes("$freeTypeCommit`n$freeTypeCommit`n"))
    Assert-CommitContract ($multipleResult.ExitCode -ne 0) `
        "multiple Git output records were accepted: $($multipleResult.Output.Trim())"

    $gitFailureResult = Invoke-CommitProbe $helperPath $msdfgenPath $msdfgenCommit `
        'msdfgen' ([byte[]]::new(0)) 17
    Assert-CommitContract ($gitFailureResult.ExitCode -ne 0) `
        "a failing Git command was accepted: $($gitFailureResult.Output.Trim())"
}
finally {
    $env:PATH = $originalPath
    $env:NODE_OPTIONS = $originalNodeOptions
    $env:VNM_TEST_GIT_OUTPUT_FILE = $originalOutputFile
    $env:VNM_TEST_GIT_EXIT_CODE = $originalExitCode
    Remove-Item -LiteralPath $script:ProbeRoot -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Output 'Portable dependency commit contract passed.'
