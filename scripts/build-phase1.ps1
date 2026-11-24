[CmdletBinding()]
param(
    [string]$Generator = 'Visual Studio 16 2019',
    [switch]$App,
    [switch]$WinUI
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$stage = Join-Path $root '.phase1'
$source = Join-Path $stage 'rnnoise'
$cmake = Join-Path $stage '.venv\Scripts\cmake.exe'
$patch = Join-Path $root 'patches\rnnoise-amd-npu.patch'
$build = Join-Path $stage 'build'
$manifestPath = Join-Path $stage 'manifest.json'
if (-not (Test-Path -LiteralPath $cmake -PathType Leaf) -or
    -not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw 'Run scripts\setup-phase1.ps1 before building.'
}
$manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
$head = & git.exe -C $source rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $head -ne $manifest.rnnoiseCommit) {
    throw 'The RNNoise checkout must be at the staged commit.'
}

$sharedProviderPatch = Join-Path $root 'patches\rnnoise-winml-shared-provider.patch'
& git.exe -C $source apply --reverse --check $sharedProviderPatch 2>$null
# The shared-provider patch modifies hunks from the original patch.
$patches = if ($LASTEXITCODE -ne 0) { @($patch, $sharedProviderPatch) } else { @() }
foreach ($patchPath in @($patches) + (Join-Path $root 'patches\rnnoise-winml-sessions.patch')) {
    & git.exe -C $source apply --reverse --check $patchPath 2>$null
    if ($LASTEXITCODE -ne 0) {
        & git.exe -C $source apply $patchPath
        if ($LASTEXITCODE -ne 0) { throw "Could not apply $patchPath. Check local RNNoise edits." }
    }
}

if ($WinUI) {
    $locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $msbuild = & $locator -latest -version '[17.0,18.0)' -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\Current\Bin\amd64\MSBuild.exe' | Select-Object -First 1
    if (-not $msbuild) { throw 'Install VS 2022 C++ Build Tools and Windows SDK 10.0.26100 to build the WinUI app.' }
    & $msbuild (Join-Path $root 'app\winui\npu-broadcast.vcxproj') -restore -m -nologo -verbosity:minimal -property:Configuration=Release -property:Platform=x64
    if ($LASTEXITCODE -ne 0) { throw 'WinUI release build failed.' }
    return
}

$definitions = @(
    '-DRNNOISE_ENABLE_WINML=ON'
    '-DRNNOISE_ENABLE_ONNX=OFF'
    '-DRNNOISE_COMPARE_RNN_BACKENDS=OFF'
    "-DRNNOISE_WINML_PACKAGE_ROOT=$stage\winml-$($manifest.winmlVersion)"
    "-DRNNOISE_ONNX_MODEL=$stage\models\rnnoise_rnn.onnx"
)
if ($App) {
    $source = Join-Path $root 'app'
    $build = Join-Path $stage 'app-build'
    if (-not (Test-Path -LiteralPath (Join-Path $stage 'models\rnnoise_split\rnnoise_host.onnx'))) {
        throw 'Run scripts\split-rnnoise-matrices.py to generate the split RNNoise models first.'
    }
}
& $cmake -S $source -B $build -G $Generator -A x64 @definitions
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
if ($App) {
    & $cmake --build $build --config Release --target npu-broadcast
} else {
    & $cmake --build $build --config Release --target rnnoise_demo rnnoise_winml_ep_tool
}
if ($LASTEXITCODE -ne 0) { throw 'Phase 1 build failed.' }
