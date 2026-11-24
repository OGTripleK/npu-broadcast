[CmdletBinding()]
param([ValidateRange(1, 600)][int]$TimeoutSeconds = 160)

$ErrorActionPreference = 'Stop'
$stage = Join-Path (Split-Path -Parent $PSScriptRoot) '.phase1'
$tool = Join-Path $stage 'build\Release\rnnoise_winml_ep_tool.exe'
$inputName = 'room-echo-wet.pcm'
$outputName = 'room-echo-npu.pcm'
$log = Join-Path $stage 'room-echo-npu.log'
$cache = Join-Path $stage 'room-echo-vitis-cache'
$modelRoot = Join-Path $stage 'models\room_echo_split'
$model = Join-Path $modelRoot 'room_echo_host.onnx'
$inputFile = Get-Item -LiteralPath (Join-Path $stage $inputName)

$processInfo = [Diagnostics.ProcessStartInfo]::new($tool)
$processInfo.UseShellExecute = $false
$processInfo.CreateNoWindow = $true
$processInfo.WorkingDirectory = $stage
$processInfo.RedirectStandardError = $true
foreach ($argument in @('room-echo', $inputName, $outputName)) { $processInfo.ArgumentList.Add($argument) }
foreach ($key in @('RNNOISE_WINML_EP_DEVICE_INDEX', 'RNNOISE_WINML_DEVICE_INDEX', 'RNNOISE_WINML_ALLOW_CPU_PARTITIONS')) {
    $processInfo.Environment.Remove($key) | Out-Null
}
$processInfo.Environment['RNNOISE_WINML_MODEL'] = $model
$processInfo.Environment['RNNOISE_WINML_MATRIX_DIR'] = $modelRoot
$processInfo.Environment['RNNOISE_WINML_REPORT_CACHE'] = $cache
$processInfo.Environment['XLNX_ONNX_EP_REPORT_FILE'] = 'operator-assignment.json'

$logFile = [IO.File]::Open($log, 'Create', 'Write', 'Read')
try {
    $probe = [Diagnostics.Process]::Start($processInfo)
    try {
        $copy = $probe.StandardError.BaseStream.CopyToAsync($logFile)
        $timedOut = -not $probe.WaitForExit($TimeoutSeconds * 1000)
        if ($timedOut) {
            try { $probe.Kill($true) } catch { if (-not $probe.HasExited) { throw } }
            $probe.WaitForExit()
        }
        $copy.GetAwaiter().GetResult() | Out-Null
        $code = $probe.ExitCode
    } finally {
        $probe.Dispose()
    }
} finally {
    $logFile.Dispose()
}
if ($timedOut) { throw "Room-echo probe timed out. See $log." }
if ($code -ne 0) { throw "Room-echo probe exited with code $code. See $log." }
if ((Get-Item -LiteralPath (Join-Path $stage $outputName)).Length -ne $inputFile.Length) {
    throw "Room-echo probe returned an unexpected sample count. See $log."
}

$projections = [Collections.Generic.HashSet[string]]::new()
foreach ($file in Get-ChildItem -LiteralPath $cache -Filter 'operator-assignment.json' -File -Recurse) {
    $report = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
    foreach ($node in $report.nodeStat) {
        if ($node.opType -eq 'Conv' -and $node.device -eq 'NPU') {
            $projections.Add($node.output[0]) | Out-Null
        }
    }
}
if ($projections.Count -ne 20) { throw "Expected 20 NPU convolution placements, found $($projections.Count)." }
Get-Content -LiteralPath $log -Tail 1
Write-Host "Verified all 20 NPU convolutions and $($inputFile.Length / 2) output samples."
