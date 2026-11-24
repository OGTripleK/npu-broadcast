[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('cpu', 'npu')][string]$Backend,
    [Parameter(Mandatory)][string]$InputPcm,
    [string]$ModelPath,
    [string]$MatrixDirectory,
    [ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$ResultName,
    [switch]$AllowCpuPartitions,
    [switch]$Paced,
    [ValidateRange(1, 1800)][int]$TimeoutSeconds = 60
)

$ErrorActionPreference = 'Stop'
if ($AllowCpuPartitions -and $Backend -ne 'npu') { throw 'CPU partitions apply to the NPU diagnostic only.' }
if ($MatrixDirectory -and ($Backend -ne 'npu' -or $AllowCpuPartitions -or $ModelPath)) {
    throw 'A matrix directory selects the split NPU model. Use it with NPU mode alone.'
}
$stage = Join-Path (Split-Path -Parent $PSScriptRoot) '.phase1'
$inputFile = Get-Item -LiteralPath $InputPcm
if ($inputFile.PSIsContainer -or $inputFile.Length -lt 1920 -or $inputFile.Length % 960 -ne 0) {
    throw 'Provide at least two complete 480-sample frames of 16-bit mono PCM.'
}
$inputPath = $inputFile.FullName
$demo = Join-Path $stage 'build\Release\rnnoise_demo.exe'
$matrixRoot = if ($MatrixDirectory) { (Resolve-Path -LiteralPath $MatrixDirectory).Path }
$model = if ($matrixRoot) { Join-Path $matrixRoot 'rnnoise_host.onnx' }
         elseif ($ModelPath) { (Resolve-Path -LiteralPath $ModelPath).Path }
         else { Join-Path $stage 'models\rnnoise_rnn.onnx' }
$label = if ($ModelPath -or $matrixRoot) { "probe-$Backend-$([IO.Path]::GetFileNameWithoutExtension($model))" }
         else { "probe-$Backend" }
if ($Paced) { $label += '-paced' }
if ($ResultName) { $label = $ResultName }
$output = Join-Path $stage "$label.pcm"
$log = Join-Path $stage "$label.log"
if ($inputPath -eq $output) { throw 'The input PCM must differ from the probe output.' }

$startInfo = [Diagnostics.ProcessStartInfo]::new($demo)
$startInfo.UseShellExecute = $false
$startInfo.WorkingDirectory = $stage
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardError = $true
$startInfo.ArgumentList.Add($inputPath)
$startInfo.ArgumentList.Add($output)
$startInfo.Environment['RNNOISE_USE_WINML'] = '1'
$startInfo.Environment['RNNOISE_WINML_MODEL'] = $model
$startInfo.Environment['RNNOISE_WINML_EP'] = $Backend
$startInfo.Environment.Remove('RNNOISE_WINML_EP_DEVICE_INDEX') | Out-Null
$startInfo.Environment.Remove('RNNOISE_WINML_DEVICE_INDEX') | Out-Null
$startInfo.Environment.Remove('RNNOISE_WINML_ALLOW_CPU_PARTITIONS') | Out-Null
$startInfo.Environment.Remove('RNNOISE_WINML_PROFILE') | Out-Null
$startInfo.Environment.Remove('RNNOISE_WINML_REPORT_CACHE') | Out-Null
$startInfo.Environment.Remove('RNNOISE_WINML_MATRIX_DIR') | Out-Null
$startInfo.Environment.Remove('RNNOISE_PCM_PACED') | Out-Null
if ($Paced) { $startInfo.Environment['RNNOISE_PCM_PACED'] = '1' }
if ($Backend -eq 'npu') {
    $startInfo.Environment['RNNOISE_WINML_REQUIRE_NPU'] = '1'
    if ($matrixRoot) {
        $startInfo.Environment['RNNOISE_WINML_MATRIX_DIR'] = $matrixRoot
        $startInfo.Environment['RNNOISE_WINML_REPORT_CACHE'] = Join-Path $stage 'vitis-cache'
        $startInfo.Environment['XLNX_ONNX_EP_REPORT_FILE'] = 'operator-assignment.json'
    }
    if ($AllowCpuPartitions) {
        $startInfo.Environment['RNNOISE_WINML_ALLOW_CPU_PARTITIONS'] = '1'
        $startInfo.Environment['RNNOISE_WINML_PROFILE'] = Join-Path $stage "$label-profile"
        $startInfo.Environment['RNNOISE_WINML_REPORT_CACHE'] = Join-Path $stage 'vitis-cache'
        $startInfo.Environment['XLNX_ONNX_EP_REPORT_FILE'] = 'operator-assignment.json'
    }
} else {
    $startInfo.Environment.Remove('RNNOISE_WINML_REQUIRE_NPU') | Out-Null
}

$logFile = [IO.File]::Open($log, 'Create', 'Write', 'Read')
try {
    $elapsed = [Diagnostics.Stopwatch]::StartNew()
    $probe = [Diagnostics.Process]::Start($startInfo)
    try {
        $copy = $probe.StandardError.BaseStream.CopyToAsync($logFile)
        $timedOut = -not $probe.WaitForExit($TimeoutSeconds * 1000)
        if ($timedOut) {
            try { $probe.Kill($true) } catch { if (-not $probe.HasExited) { throw } }
            $probe.WaitForExit()
        }
        $copy.GetAwaiter().GetResult() | Out-Null
        $code = $probe.ExitCode
        $elapsed.Stop()
        $cpuMs = $null
        if ($Paced) {
            try { $cpuMs = $probe.TotalProcessorTime.TotalMilliseconds } catch { }
        }
    } finally {
        $probe.Dispose()
    }
} finally {
    $logFile.Dispose()
}

if ($timedOut) { throw "$Backend probe timed out after $TimeoutSeconds seconds. See $log." }
if ($code -ne 0) { throw "$Backend probe exited with code $code. See $log." }
if ((Get-Item -LiteralPath $output).Length -ne ($inputFile.Length - 960)) {
    throw "$Backend probe produced an unexpected frame count. See $log."
}
if ($Paced) {
    $processing = [Collections.Generic.List[double]]::new()
    $completion = [Collections.Generic.List[double]]::new()
    $cold = $null
    $late = 0
    foreach ($line in [IO.File]::ReadLines($log)) {
        if ($line.StartsWith('cold_us ')) {
            $cold = [double]::Parse($line.Substring(8), [Globalization.CultureInfo]::InvariantCulture) / 1000
        } elseif ($line.StartsWith('frame_us ')) {
            $fields = $line.Split(' ')
            $processing.Add([double]::Parse($fields[1], [Globalization.CultureInfo]::InvariantCulture) / 1000)
            $duration = [double]::Parse($fields[2], [Globalization.CultureInfo]::InvariantCulture) / 1000
            $completion.Add($duration)
            if ($duration -gt 10) { $late++ }
        }
    }
    $expected = ($inputFile.Length / 960) - 1
    if ($null -eq $cold -or $processing.Count -ne $expected) {
        throw "Incomplete paced frame timings. See $log."
    }
    $lastCompletion = $completion[$completion.Count - 1]
    $processing.Sort()
    $completion.Sort()
    $p95Index = [int][Math]::Ceiling($expected * 0.95) - 1
    $summary = [ordered]@{
        coldMs = $cold
        warmFrames = $processing.Count
        meanProcessingMs = ($processing | Measure-Object -Average).Average
        p95ProcessingMs = $processing[$p95Index]
        p95CompletionMs = $completion[$p95Index]
        missedDeadlines = $late
        maxCompletionMs = $completion[$completion.Count - 1]
        finalCompletionMs = $lastCompletion
        processCpuMs = $cpuMs
        elapsedMs = $elapsed.Elapsed.TotalMilliseconds
    }
    $summary | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stage "$label-metrics.json")
    Write-Host ("P95 processing {0:F2} ms, {1} of {2} warm frames completed late." -f
        $summary.p95ProcessingMs, $late, $expected)
}
Write-Host "$Backend probe wrote $output. See $log."
