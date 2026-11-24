[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$WdkRoot,
    [string]$SourceCheckout
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$stage = Join-Path $root '.phase1'
if (-not $SourceCheckout) { $SourceCheckout = Join-Path $stage 'sysvad-upstream' }
$source = Join-Path $stage 'sysvad-npu'
$build = Join-Path $stage 'npu-mic-kernel'
$objects = Join-Path $build 'obj'
$package = Join-Path $build 'unsigned'
New-Item -ItemType Directory -Path $objects,$package -Force | Out-Null

$kit = (Resolve-Path -LiteralPath $WdkRoot).Path
$version = '10.0.26100.0'
$sdk = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\Include\$version"
$locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $locator -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (-not $vs) { throw 'Install the VS 2022 x64 C++ Build Tools.' }
$vc = Get-ChildItem -LiteralPath (Join-Path $vs 'VC\Tools\MSVC') -Directory |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1 -ExpandProperty FullName
if (-not $vc) { throw 'VS 2022 C++ compiler was not found.' }

& python (Join-Path $root 'driver\prepare-sysvad.py') $SourceCheckout $source
if ($LASTEXITCODE -ne 0) { throw 'Could not prepare the pinned SysVAD source.' }
$sysvad = Join-Path $source 'audio\sysvad'
$includes = @("$kit/Include/$version/km", "$kit/Include/$version/km/crt", "$kit/Include/wdf/kmdf/1.33",
              "$sdk/shared", "$sdk/um", "$vc/include", $sysvad, "$sysvad/EndpointsCommon", "$sysvad/TabletAudioSample")
$flags = $includes | ForEach-Object { "/I$_" }
$sources = foreach ($project in @('TabletAudioSample/TabletAudioSample', 'EndpointsCommon/EndpointsCommon')) {
    $proj = Join-Path $sysvad "$project.vcxproj"
    $dir = Split-Path $proj
    Select-Xml -Path $proj -XPath "//*[local-name()='ClCompile'][@Include]" |
        ForEach-Object { Join-Path $dir $_.Node.Include }
}
& "$vc/bin/Hostx64/x64/cl.exe" /nologo /c /std:c++17 /kernel /O2 /GS /MD /D_AMD64_ /DAMD64 /D_WIN64 /DUNICODE /D_UNICODE /DPC_IMPLEMENTATION /DSYSVAD_BTH_BYPASS /DSYSVAD_USB_SIDEBAND /D_USE_WAVERT_ /D_NEW_DELETE_OPERATORS_ /D_USE_IPortClsRuntimePower /DNTDDI_VERSION=0x0A000010 /DKMDF_VERSION_MAJOR=1 /DKMDF_VERSION_MINOR=33 "/Fo$objects/" @flags @sources
if ($LASTEXITCODE -ne 0) { throw 'Driver compilation failed.' }

$kmLib = "$kit/Lib/$version/km/x64"
$wdfLib = "$kit/Lib/wdf/kmdf/x64/1.33"
$compiled = $sources | ForEach-Object { Join-Path $objects ([IO.Path]::GetFileNameWithoutExtension($_) + '.obj') }
& "$vc/bin/Hostx64/x64/link.exe" /NOLOGO /NODEFAULTLIB /DRIVER /ENTRY:FxDriverEntry /SUBSYSTEM:NATIVE /MACHINE:X64 /INTEGRITYCHECK "/OUT:$package/TabletAudioSample.sys" "/LIBPATH:$kmLib" "/LIBPATH:$wdfLib" @compiled ntoskrnl.lib hal.lib portcls.lib stdunk.lib libcntpr.lib wdmsec.lib BufferOverflowFastFailK.lib wdfdriverentry.lib wdfldr.lib
if ($LASTEXITCODE -ne 0) { throw 'Driver link failed.' }

$inf = Join-Path $package 'NpuBroadcastMic.inf'
Copy-Item -LiteralPath (Join-Path $root 'driver\NpuBroadcastMic.inx') -Destination $inf -Force
& "$kit/bin/$version/x64/stampinf.exe" -f $inf -a amd64 -k 1.33 -d 08/01/2025 -v 0.1.0.0
if ($LASTEXITCODE -ne 0) { throw 'Could not stamp the INF.' }
& "$kit/tools/$version/x64/infverif.exe" /h $inf
if ($LASTEXITCODE -ne 0) { throw 'INF verification failed.' }
Copy-Item -LiteralPath (Join-Path $source 'LICENSE') -Destination (Join-Path $package 'Windows-driver-samples-LICENSE.txt') -Force
Write-Output $package
