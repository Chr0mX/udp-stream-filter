[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo',
    [switch] $Installer
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( $env:CI -eq $null ) {
    throw "Package-Windows.ps1 requires CI environment"
}

if ( ! ( [System.Environment]::Is64BitOperatingSystem ) ) {
    throw "Packaging script requires a 64-bit system to build and run."
}

if ( $PSVersionTable.PSVersion -lt '7.2.0' ) {
    Write-Warning 'The packaging script requires PowerShell Core 7. Install or upgrade your PowerShell version: https://aka.ms/pscore6'
    exit 2
}

function Package {
    trap {
        Write-Error $_
        exit 2
    }

    $ScriptHome = $PSScriptRoot
    $ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."
    $BuildSpecFile = "${ProjectRoot}/buildspec.json"

    $UtilityFunctions = Get-ChildItem -Path $PSScriptRoot/utils.pwsh/*.ps1 -Recurse

    foreach( $Utility in $UtilityFunctions ) {
        Write-Debug "Loading $($Utility.FullName)"
        . $Utility.FullName
    }

    $BuildSpec = Get-Content -Path ${BuildSpecFile} -Raw | ConvertFrom-Json
    $ProductName = $BuildSpec.name
    $ProductVersion = $BuildSpec.version

    $OutputName = "${ProductName}-${ProductVersion}-windows-${Target}"

    $RemoveArgs = @{
        ErrorAction = 'SilentlyContinue'
        Path = @(
            "${ProjectRoot}/release/${ProductName}-*-windows-*.zip"
        )
    }

    Remove-Item @RemoveArgs

    Log-Group "Archiving ${ProductName}..."
    $CompressArgs = @{
        Path = (Get-ChildItem -Path "${ProjectRoot}/release/${Configuration}" -Exclude "${OutputName}*.*")
        CompressionLevel = 'Optimal'
        DestinationPath = "${ProjectRoot}/release/${OutputName}.zip"
        Verbose = ($Env:CI -ne $null)
    }
    Compress-Archive -Force @CompressArgs
    Log-Group

    if ( $Installer ) {
        Log-Group "Building Windows installer for ${ProductName}..."

        $IsccCandidates = @(
            "${Env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
            "${Env:ProgramFiles}\Inno Setup 6\ISCC.exe"
        )
        $Iscc = $IsccCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1

        if ( -not $Iscc ) {
            $IsccCommand = Get-Command ISCC.exe -ErrorAction SilentlyContinue
            if ( $IsccCommand ) {
                $Iscc = $IsccCommand.Source
            }
        }

        if ( -not $Iscc ) {
            throw "Inno Setup (ISCC.exe) was not found. Install it from https://jrsoftware.org/isinfo.php or via 'choco install innosetup'."
        }

        # RundirConfig must match the build's actual configuration -- a
        # version-tag-triggered release build compiles in "Release" rather
        # than the usual "RelWithDebInfo" (see build-project.yaml's
        # check-event job), and installer.iss has no way to know that on
        # its own. Without this, ISCC looks for the built DLL under the
        # wrong rundir\<config>\ folder and fails outright.
        Invoke-External $Iscc "/DPluginName=${ProductName}" "/DPluginVersion=${ProductVersion}" "/DRundirConfig=${Configuration}" "${ProjectRoot}/installer.iss"
        Log-Group
    }
}

Package
