[CmdletBinding()]
param([switch]$CMakePath)

$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = @()
if (Test-Path -LiteralPath $vswhere) {
    $vs = @(& $vswhere -all -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json |
        ConvertFrom-Json | Sort-Object { [version]$_.installationVersion } -Descending)
}

$cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
$cmakeExe = $cmake.Source
if (-not $cmakeExe) {
    foreach ($install in $vs) {
        $candidate = Join-Path $install.installationPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $cmakeExe = $candidate
            break
        }
    }
}
if ($CMakePath) {
    if (-not $cmakeExe) { throw 'CMake was not found on PATH or in Visual Studio.' }
    $cmakeExe
    return
}

$win = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
Write-Host "Windows $($win.DisplayVersion), build $($win.CurrentBuild).$($win.UBR)"
$bios = Get-ItemProperty 'HKLM:\HARDWARE\DESCRIPTION\System\BIOS'
$cpu = Get-ItemPropertyValue 'HKLM:\HARDWARE\DESCRIPTION\System\CentralProcessor\0' 'ProcessorNameString'
Write-Host "$($bios.SystemProductName) / $($cpu.Trim())"

$devices = & pnputil.exe /enum-devices /connected /class ComputeAccelerator
if ($LASTEXITCODE -ne 0) {
    Write-Warning 'Connected accelerator inventory is unavailable.'
}
$devices | Write-Host
foreach ($line in $devices) {
    if ($line -match '(PCI\\VEN_1022&\S+)\s*$') {
        $instance = $Matches[1]
        try {
            $driverKey = Get-ItemPropertyValue -LiteralPath "HKLM:\SYSTEM\CurrentControlSet\Enum\$instance" -Name Driver
            $driver = Get-ItemProperty -LiteralPath "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$driverKey"
            Write-Host "AMD accelerator driver $($driver.DriverVersion) ($($driver.InfPath))"
        } catch {
            Write-Warning "AMD accelerator driver inventory is unavailable. $($_.Exception.Message)"
        }
    }
}
try {
    $providerPackages = @(Get-AppxPackage -Name '*Vitis*' -ErrorAction Stop)
    if ($providerPackages) {
        foreach ($package in $providerPackages) { Write-Host "$($package.Name) $($package.Version)" }
    } else {
        Write-Host 'No Vitis-named Appx package for this user. WinML catalog availability is untested.'
    }
} catch {
    Write-Warning "VitisAI Appx inventory is unavailable. $($_.Exception.Message)"
}

if ($vs) {
    foreach ($install in $vs) {
        Write-Host "MSVC x64 workload: $($install.displayName) $($install.installationVersion) at $($install.installationPath)"
    }
} else {
    Write-Warning 'No Visual Studio instance with the x86/x64 C++ toolset was found.'
}

$kits = Get-ItemPropertyValue 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' 'KitsRoot10' -ErrorAction SilentlyContinue
$sdks = @()
if ($kits) {
    $sdks = @(Get-ChildItem -Directory -LiteralPath (Join-Path $kits 'Include') -ErrorAction SilentlyContinue |
        Where-Object {
            (Test-Path -LiteralPath (Join-Path $_.FullName 'um\Windows.h')) -and
            (Test-Path -LiteralPath (Join-Path $kits "Lib\$($_.Name)\um\x64\kernel32.lib"))
        } | Select-Object -ExpandProperty Name)
}
if ($sdks) { Write-Host "Windows SDK (x64): $($sdks -join ', ')" }
else { Write-Warning 'No Windows SDK with x64 headers and libraries was found.' }

if ($cmakeExe) {
    Write-Host "CMake: $cmakeExe"
    & $cmakeExe --version | Select-Object -First 1 | Write-Host
} else { Write-Warning 'CMake was not found.' }

if (Get-Command py.exe -ErrorAction SilentlyContinue) {
    & py.exe -0p | Write-Host
} elseif (Get-Command python.exe -ErrorAction SilentlyContinue) {
    & python.exe --version | Write-Host
} else { Write-Warning 'Python was not found.' }
