[CmdletBinding()]
param(
    [ValidatePattern('^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$')]
    [string]$Version,
    [string]$BuildPath
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$stage = Join-Path $root '.phase1'
if ($Version) {
    $releaseFolder = Join-Path $root "releases\v$Version"
    if (Test-Path -LiteralPath $releaseFolder) { throw "Release v$Version already exists. Use a new version." }
}

if ($BuildPath) {
    $release = (Resolve-Path -LiteralPath $BuildPath).Path
} else {
    $release = Join-Path $root 'app\winui\bin\Release'
    & (Join-Path $PSScriptRoot 'build-phase1.ps1') -WinUI
}

foreach ($model in @('rnnoise_host.onnx', 'room-echo\room_echo_host.onnx')) {
    if (-not (Test-Path -LiteralPath (Join-Path $release "models\$model") -PathType Leaf)) {
        throw "Missing release model $model."
    }
}
foreach ($directory in @('models', 'models\room-echo')) {
    $expected = if ($directory -eq 'models') { 10 } else { 20 }
    for ($index = 0; $index -lt $expected; $index++) {
        if (-not (Test-Path -LiteralPath (Join-Path $release "$directory\matrix-$index.onnx") -PathType Leaf)) {
            throw "Missing release convolution $directory\matrix-$index.onnx."
        }
    }
}

$sources = @(Get-ChildItem -LiteralPath $release -Exclude *.pdb,*.exp,*.lib | Select-Object -ExpandProperty FullName)
$sources += Join-Path $root 'app\OPEN-ME.txt'
$zip = Join-Path $stage 'NPU-Broadcast-Windows-x64.zip'
$pending = Join-Path $stage "NPU-Broadcast-$([guid]::NewGuid()).zip"
Compress-Archive -LiteralPath $sources -DestinationPath $pending
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($pending)
try {
    foreach ($name in @('NpuBroadcast.exe', 'NpuBroadcast.pri', 'MainWindow.xbf', 'Microsoft.ui.xaml.dll',
            'OPEN-ME.txt', 'RNNoise-COPYING.txt', 'LocalVQE-LICENSE.txt', 'WinML-license.txt',
            'WinML-ThirdPartyNotices.txt', 'WindowsAppSDK-license.txt', 'WindowsAppSDK-NOTICE.txt',
            'models/rnnoise_host.onnx', 'models/room-echo/room_echo_host.onnx')) {
        $entry = $archive.GetEntry($name)
        if ($null -eq $entry) { $entry = $archive.GetEntry($name.Replace('/', '\')) }
        if ($null -eq $entry -or $entry.Length -eq 0) {
            throw "The archive is missing $name."
        }
    }
} finally {
    $archive.Dispose()
}
if ($Version) {
    $stagingFolder = Join-Path $stage "release-$([guid]::NewGuid())"
    [IO.Compression.ZipFile]::ExtractToDirectory($pending, $stagingFolder)
    New-Item -ItemType Directory -Path (Split-Path -Parent $releaseFolder) -Force | Out-Null
    [IO.Directory]::Move($stagingFolder, $releaseFolder)
    Remove-Item -LiteralPath $pending
    Get-FileHash -LiteralPath (Join-Path $releaseFolder 'NpuBroadcast.exe') -Algorithm SHA256 |
        Select-Object Path,Hash
} else {
    Move-Item -LiteralPath $pending -Destination $zip -Force
    Get-FileHash -LiteralPath $zip -Algorithm SHA256 | Select-Object Path,Hash
}
