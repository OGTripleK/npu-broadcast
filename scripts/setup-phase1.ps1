[CmdletBinding()]
param([switch]$RoomEcho)

$ErrorActionPreference = 'Stop'
$commit = '91fca7abe74c4b4fb233b049e7e4dae17b1ebbb6'
$fork = 'https://github.com/sorayuki/rnnoise.git'
$winmlVersion = '2.5.96'
$cmakePin = 'cmake==3.31.6'
$winmlHash = '0903EFA70C5C829593D6C99251E974FE91184F9B42737D6C6731A0B3D23907E77587B7258EAB1DE47394484206F6E91E6FDB36E589D9BCC88726CC06C3209C99'

function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program $($Arguments -join ' ') failed with exit code $LASTEXITCODE."
    }
}

function Get-VerifiedDownload {
    param([string]$Url, [string]$Path, [string]$Algorithm, [string]$Hash)
    $fileToCheck = $Path
    if (-not (Test-Path -LiteralPath $Path)) {
        $fileToCheck = "$Path.partial"
        if ((-not (Test-Path -LiteralPath $fileToCheck)) -or
            ((Get-FileHash -Algorithm $Algorithm -LiteralPath $fileToCheck).Hash -ne $Hash)) {
            Invoke-Checked 'curl.exe' @('--fail', '--location', '--silent', '--show-error', '--retry', '2',
                '--connect-timeout', '30', '--max-time', '300', '--output', $fileToCheck, $Url)
        }
    }
    if ((Get-FileHash -Algorithm $Algorithm -LiteralPath $fileToCheck).Hash -ne $Hash) {
        throw "$Algorithm mismatch for $fileToCheck. Expected $Hash. Check the archive before retrying."
    }
    if ($fileToCheck -ne $Path) {
        Rename-Item -LiteralPath $fileToCheck -NewName ([IO.Path]::GetFileName($Path))
    }
}

function Initialize-PinnedCheckout {
    param([string]$Directory, [string]$Repository, [string]$Revision)
    if (-not (Test-Path -LiteralPath (Join-Path $Directory '.git'))) {
        Invoke-Checked 'git.exe' @('init', $Directory) | Out-Host
    }
    $head = & git.exe -C $Directory rev-parse --verify --quiet HEAD
    if ($LASTEXITCODE -ne 0) {
        Invoke-Checked 'git.exe' @('-C', $Directory, 'fetch', '--depth=1', $Repository, $Revision) | Out-Host
        Invoke-Checked 'git.exe' @('-C', $Directory, 'checkout', '--detach', $Revision) | Out-Host
        $head = Invoke-Checked 'git.exe' @('-C', $Directory, 'rev-parse', 'HEAD')
    }
    if ($head -ne $Revision) {
        throw "$Directory is at $head. Expected $Revision. Existing checkout preserved."
    }
    $head
}

foreach ($program in @('git.exe', 'curl.exe', 'tar.exe', 'py.exe')) {
    if (-not (Get-Command $program -ErrorAction SilentlyContinue)) {
        throw "$program is required. Run scripts\preflight.ps1 to check the local setup."
    }
}
Invoke-Checked 'py.exe' @('-3.13', '-c', "import sysconfig; assert sysconfig.get_platform() == 'win-amd64', 'Install x64 Python 3.13'")

$stage = Join-Path (Split-Path -Parent $PSScriptRoot) '.phase1'
$downloads = Join-Path $stage 'downloads'
$source = Join-Path $stage 'rnnoise'
$weights = Join-Path $stage 'weights'
$winml = Join-Path $stage "winml-$winmlVersion"
$venv = Join-Path $stage '.venv'
$modelDir = Join-Path $stage 'models'
$model = Join-Path $modelDir 'rnnoise_rnn.onnx'
$receipt = Join-Path $stage 'manifest.json'
New-Item -ItemType Directory -Force -Path $stage | Out-Null
try {
    $lock = [IO.File]::Open((Join-Path $stage 'setup.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
} catch {
    throw "Cannot lock $stage for setup. $($_.Exception.Message)"
}
try {
    New-Item -ItemType Directory -Force -Path $downloads, $weights, $modelDir | Out-Null
    $head = Initialize-PinnedCheckout $source $fork $commit
    Invoke-Checked 'git.exe' @('-C', $source, 'diff', '--quiet', 'HEAD', '--', 'model_version', 'scripts/export_rnn_onnx.py')

    $weightHash = (Get-Content -LiteralPath (Join-Path $source 'model_version') -Raw).Trim()
    if ($weightHash -notmatch '^[0-9a-f]{64}$') { throw 'Invalid SHA-256 in RNNoise model_version.' }
    $weightName = "rnnoise_data-$weightHash.tar.gz"
    $weightArchive = Join-Path $downloads $weightName
    Get-VerifiedDownload "https://media.xiph.org/rnnoise/models/$weightName" $weightArchive 'SHA256' $weightHash
    $weightFiles = @('src/rnnoise_data.c', 'src/rnnoise_data.h')
    $archiveMembers = @(Invoke-Checked 'tar.exe' @('-tf', $weightArchive))
    $members = foreach ($file in $weightFiles) {
        $member = $archiveMembers | Where-Object { $_ -ceq $file -or $_ -ceq "./$file" } | Select-Object -First 1
        if (-not $member) { throw "$weightArchive is missing $file." }
        $member
    }
    Invoke-Checked 'tar.exe' (@('-xf', $weightArchive, '-C', $weights) + $members)
    foreach ($file in $weightFiles) {
        $original = Join-Path $weights $file
        $target = Join-Path $source $file
        if (-not (Test-Path -LiteralPath $target)) {
            Copy-Item -LiteralPath $original -Destination $target
        } elseif ((Get-FileHash -LiteralPath $original).Hash -ne (Get-FileHash -LiteralPath $target).Hash) {
            throw "$target differs from the verified Xiph weights. Existing file preserved."
        }
    }

    $pins = @('numpy==2.3.5', 'onnx==1.20.1', 'protobuf==6.33.2', 'typing-extensions==4.15.0', 'ml-dtypes==0.5.4')
    $python = Join-Path $venv 'Scripts\python.exe'
    if (-not (Test-Path -LiteralPath $python)) {
        Invoke-Checked 'py.exe' @('-3.13', '-m', 'venv', $venv)
    }
    Invoke-Checked $python (@('-m', 'pip', '--isolated', '--disable-pip-version-check', 'install',
        '--cache-dir', (Join-Path $stage 'pip-cache'), '--index-url', 'https://pypi.org/simple', '--only-binary=:all:') + $pins + $cmakePin)
    Invoke-Checked $python @('-m', 'pip', '--disable-pip-version-check', 'check')

    if (-not (Test-Path -LiteralPath $model)) {
        $pendingModel = "$model.partial"
        Invoke-Checked $python @((Join-Path $source 'scripts/export_rnn_onnx.py'),
            '--data-c', (Join-Path $source 'src/rnnoise_data.c'), '--output', $pendingModel, '--no-optimize')
        Rename-Item -LiteralPath $pendingModel -NewName ([IO.Path]::GetFileName($model))
    }
    $modelHash = (Get-FileHash -LiteralPath $model).Hash
    if (Test-Path -LiteralPath $receipt) {
        $previous = Get-Content -LiteralPath $receipt -Raw | ConvertFrom-Json
        if ($modelHash -ne $previous.onnxSha256) { throw "SHA-256 mismatch for $model. Existing model preserved." }
    }
    Invoke-Checked $python @('-c', 'import onnx, sys; onnx.checker.check_model(sys.argv[1])', $model)

    $winmlArchive = Join-Path $downloads "winml-$winmlVersion.nupkg"
    $id = 'microsoft.windows.ai.machinelearning'
    Get-VerifiedDownload "https://api.nuget.org/v3-flatcontainer/$id/$winmlVersion/$id.$winmlVersion.nupkg" $winmlArchive 'SHA512' $winmlHash
    if (-not (Test-Path -LiteralPath $winml)) {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $pendingWinML = "$winml.extracting-$([guid]::NewGuid())"
        [IO.Compression.ZipFile]::ExtractToDirectory($winmlArchive, $pendingWinML)
        Rename-Item -LiteralPath $pendingWinML -NewName ([IO.Path]::GetFileName($winml))
    }
    $requiredFiles = @(
        "build/cmake/$id-config.cmake"
        "build/cmake/$id-targets.cmake"
        'include/WinMLEpCatalog.h'
        'include/winml/onnxruntime_c_api.h'
        'lib/native/x64/Microsoft.Windows.AI.MachineLearning.lib'
        'lib/native/x64/onnxruntime.lib'
        'runtimes/win-x64/native/Microsoft.Windows.AI.MachineLearning.dll'
        'runtimes/win-x64/native/onnxruntime.dll'
        'runtimes/win-x64/native/DirectML.dll'
        'license.txt'
        'ThirdPartyNotices.txt'
    )
    foreach ($file in $requiredFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $winml $file) -PathType Leaf)) {
            throw "WinML $winmlVersion is missing $file under $winml."
        }
    }

    $manifest = [ordered]@{
        rnnoiseCommit = $head
        weightsSha256 = $weightHash
        pythonVersion = Invoke-Checked $python @('--version')
        exportRequirements = $pins
        buildRequirement = $cmakePin
        onnxSha256 = $modelHash
        winmlVersion = $winmlVersion
        winmlPackageSha512 = $winmlHash
        licensing = @(
            'rnnoise/COPYING'
            "winml-$winmlVersion/license.txt"
            "winml-$winmlVersion/ThirdPartyNotices.txt"
            'https://learn.microsoft.com/windows/ai/new-windows-ml/third-party-notices'
        )
    } | ConvertTo-Json
    if ((-not (Test-Path -LiteralPath $receipt)) -or ((Get-Content -LiteralPath $receipt -Raw).Trim() -ne $manifest)) {
        $manifest | Set-Content -LiteralPath $receipt -Encoding UTF8
    }
    Write-Host "Phase 1 staged. Model SHA-256: $modelHash"
    Write-Host "Dependency and license record: $receipt"
    if ($RoomEcho) {
        $echoSource = Join-Path $stage 'localvqe'
        $echoCommit = '7f61cd05095d42e56a135b303f5bb4d761da22b9'
        $echoModel = 'localvqe-v1.2-1.3M.pt'
        $echoWeightsRevision = '29ca38495cba9d6393a92a4dd890f28dd81f758d'
        $echoHash = 'ff6885e7c8d7d29a8ce963303dcd668ae0f2a7bdafae28631292fe6f06f7cd77'
        $echoUrl = "https://huggingface.co/LocalAI-io/LocalVQE/resolve/$echoWeightsRevision/$echoModel"
        Initialize-PinnedCheckout $echoSource 'https://github.com/localai-org/LocalVQE.git' $echoCommit | Out-Null
        Get-VerifiedDownload $echoUrl (Join-Path $downloads $echoModel) 'SHA256' $echoHash
        Invoke-Checked $python @('-m', 'pip', '--isolated', '--disable-pip-version-check', 'install',
            '--cache-dir', (Join-Path $stage 'pip-cache'), '--index-url', 'https://pypi.org/simple',
            '--extra-index-url', 'https://download.pytorch.org/whl/cpu', '--only-binary=:all:',
            'torch==2.10.0+cpu', 'einops==0.8.2')
        Write-Host 'Staged the LocalVQE v1.2 reference model for room-echo tests.'
    }
} finally {
    $lock.Dispose()
}
