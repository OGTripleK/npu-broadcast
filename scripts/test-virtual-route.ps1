[CmdletBinding()]
param(
    [Parameter(Mandatory)][uint32]$SourcePlayback,
    [Parameter(Mandatory)][uint32]$VirtualPlayback,
    [Parameter(Mandatory)][uint32]$VirtualRecording,
    [Parameter(Mandatory)][ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$ResultName,
    [Parameter(Mandatory)][string]$Fixture,
    [switch]$RoomEcho,
    [switch]$Bypass
)

$ErrorActionPreference = 'Stop'
if ($SourcePlayback -eq $VirtualPlayback) { throw 'Use distinct virtual playback endpoints.' }
if ($RoomEcho -and $Bypass) { throw 'Choose either RoomEcho or Bypass.' }
$stage = Join-Path (Split-Path -Parent $PSScriptRoot) '.phase1'
$app = Join-Path $stage 'app-build\Release\npu-broadcast.exe'
$fixtureFile = Get-Item -LiteralPath $Fixture
if ($fixtureFile.Length -lt 96000 -or $fixtureFile.Length -gt 768000 -or $fixtureFile.Length % 2) {
    throw 'Use synthetic 48 kHz mono S16 PCM lasting 1–8 seconds.'
}
$result = Join-Path $stage $ResultName
$stdout = "$result.stdout"
$stderr = "$result.stderr"
foreach ($file in @($stdout, $stderr, "$result.source.packets", "$result.sink.packets")) {
    if (Test-Path -LiteralPath $file) { throw "Use a fresh result name. $file exists." }
}
$duration = [int][Math]::Ceiling($fixtureFile.Length / 96000) + 6
$argsForRoute = @('run', $SourcePlayback, $VirtualPlayback, $duration, '--loopback')
if ($RoomEcho) { $argsForRoute += '--room-echo' }
if ($Bypass) { $argsForRoute += '--bypass' }
$cache = if ($RoomEcho) { 'room-echo-vitis-cache' } else { 'vitis-cache' }
$startInfo = @{
    FilePath = $app
    ArgumentList = $argsForRoute
    WorkingDirectory = $stage
    WindowStyle = 'Hidden'
    Environment = @{RNNOISE_WINML_REPORT_CACHE = (Join-Path $stage $cache)}
    RedirectStandardOutput = $stdout
    RedirectStandardError = $stderr
    PassThru = $true
}
$route = $null
try {
    $route = Start-Process @startInfo
    $until = (Get-Date).AddSeconds(140)
    do {
        if ($route.HasExited) { throw "Audio route exited before playback. See $stderr." }
        if ((Get-Content -Raw -LiteralPath $stdout) -match 'Press Ctrl\+C to stop\.') { break }
        if ((Get-Date) -ge $until) { throw "Audio route did not initialize. See $stderr." }
        Start-Sleep -Milliseconds 200
    } while ($true)
    & $app route-probe $SourcePlayback $VirtualRecording $fixtureFile.FullName $result
    if ($LASTEXITCODE -ne 0) { throw "Audio endpoint probe exited with code $LASTEXITCODE." }
    if (-not $route.WaitForExit(14000)) { throw 'Audio route did not stop at its configured duration.' }
    Get-Content -LiteralPath $stdout
    if ($route.ExitCode -ne 0) { throw "Audio route exited with code $($route.ExitCode). See $stderr." }
} finally {
    if ($route) {
        if (-not $route.HasExited) { $route.Kill(); $route.WaitForExit() }
        $route.Dispose()
    }
}
