# NPU Broadcast

NPU Broadcast is a Windows app for background noise and room echo removal on the AMD XDNA 2 NPU. The desktop app routes your microphone through a dedicated Windows recording device named **Microphone (NPU Broadcast)**. See [Windows app](#windows-app) to build the executable or use the packaged ZIP. The next measurement is full-route latency through a virtual microphone.

## Setup

Run from the project root in PowerShell 7.

```powershell
.\scripts\preflight.ps1
.\scripts\setup-phase1.ps1
.\scripts\build-phase1.ps1
```

Setup requires Git, curl, tar, and the x64 Python 3.13 launcher. It fetches the pinned fork, verifies the Xiph weights and Microsoft NuGet archive, exports the ONNX model, and installs CMake 3.31.6 in the local virtual environment. WinML requires CMake 3.21 or newer. Downloads and generated files live in `.phase1/`. Rerunning setup reuses verified downloads and keeps local checkout edits. Keep the vendor checkout on the pinned commit during setup. Setup stops if the model exporter, model version file, or extracted weights have changed.

- Source checkout at `.phase1/rnnoise/`, pinned to `91fca7abe74c4b4fb233b049e7e4dae17b1ebbb6`. [RNNoise fork](https://github.com/sorayuki/rnnoise/tree/91fca7abe74c4b4fb233b049e7e4dae17b1ebbb6)
- Exported model at `.phase1/models/rnnoise_rnn.onnx` and native WinML package at `.phase1/winml-2.5.96/`. `.phase1/manifest.json` records versions, artifact hashes, and license references. [WinML package](https://www.nuget.org/packages/Microsoft.Windows.AI.MachineLearning/2.5.96)
- Check VitisAI eligibility using the local WinML catalog and NPU driver results. [Microsoft provider requirements](https://learn.microsoft.com/en-us/windows/ai/new-windows-ml/supported-execution-providers)

## Phase 1 execution

1. The build script applies `patches/rnnoise-amd-npu.patch` and builds the x64 demo. Strict NPU mode prepares only VitisAI and requires an AMD NPU. CPU mode skips catalog registration and runs the same ONNX model.

2. List providers in a normal Windows user terminal. If VitisAI appears as `NotPresent`, request just that provider and check that its state changes to `Ready`. A sandboxed terminal may return an empty catalog.

   ```powershell
   $stage = (Resolve-Path .\.phase1).Path
   & "$stage\build\Release\rnnoise_winml_ep_tool.exe" list
   # Run only if VitisAIExecutionProvider is listed as NotPresent.
   & "$stage\build\Release\rnnoise_winml_ep_tool.exe" ensure VitisAIExecutionProvider
   ```

3. Put a local clip at `.phase1/input.pcm`. Use 48 kHz mono signed 16-bit little-endian PCM with complete 480-sample frames, noisy speech, and a noise-only interval. Pure silence skips network inference. Use one `DenoiseState` across frames and run each backend in a fresh process. If VitisAI is absent or the driver is rejected, record the catalog and driver output before changing runtimes.

   ```powershell
   .\scripts\test-phase1.ps1 -Backend cpu -InputPcm .phase1\input.pcm
   .\scripts\test-phase1.ps1 -Backend npu -InputPcm .phase1\input.pcm
   ```

4. Save AMD's operator-assignment report or equivalent device trace with successful frame execution logs. Identify the matrix operations from the decomposed recurrent layers. A provider-selection log alone cannot establish placement. Use AMD diagnostics supported by the installed EP, then turn profiling off for timing. [Model exporter](https://github.com/sorayuki/rnnoise/blob/91fca7abe74c4b4fb233b049e7e4dae17b1ebbb6/scripts/export_rnn_onnx.py) · [AMD placement diagnostics](https://ryzenai.docs.amd.com/en/latest/modelrun.html)

## Hardware test results

WinML installed VitisAI and enumerated the AMD NPU at PCI device `0x17F0`. The x64 CPU probe processed 200 input frames and wrote 199 output frames. FP32 RNNoise logged `Invalid NodeIndex` during session creation and wrote zero frames. Disabling ORT graph optimization reproduced the error and hit the 45-second timeout.

The single-convolution models from `scripts/create-npu-canary.py` isolate the compiler. The FP32 run reached the BF16 compiler and failed at Peano's VFS load. The INT8 QDQ run with explicit convolution attributes returned the expected output through VitisAI with ORT's default CPU fallback disabled. AMD's compiler assigned a dequantization step to CPU inside its provider. See `.phase1/canary-npu.log` and `.phase1/canary-int8-explicit-npu.log`.

## Quantized RNNoise experiment

Use an input clip containing speech and representative background noise. The calibration option records feature and recurrent-state tensors from a CPU run. The quantizer uses those frames to create an INT8 QDQ model. Use `--convolutions` to express matrix multiplications as 1×1 convolutions.

```powershell
$python = '.\.phase1\.venv\Scripts\python.exe'
& $python -m pip install onnxruntime==1.24.4
$stage = (Resolve-Path .\.phase1).Path
$env:RNNOISE_WINML_CALIBRATION = "$stage\calibration.f32"
.\scripts\test-phase1.ps1 -Backend cpu -InputPcm .phase1\input.pcm
Remove-Item Env:RNNOISE_WINML_CALIBRATION
$candidate = '.phase1\models\rnnoise_conv_int8.onnx'
& $python .\scripts\quantize-rnnoise.py .phase1\models\rnnoise_rnn.onnx .phase1\calibration.f32 $candidate --convolutions
.\scripts\test-phase1.ps1 -Backend npu -InputPcm .phase1\input.pcm -ModelPath $candidate
```

Plain INT8 MatMul quantization passed the CPU smoke run. The strict AMD run rejected nodes assigned to the default CPU EP. A diagnostic run with `-AllowCpuPartitions` wrote all 1,312 expected frames. The ORT profile attributed quantize and dequantize events to VitisAI and all 13,130 MatMul events to CPU. Its profiled P95 model time was 9.93 ms, with CPU performing the recurrent math. The full graph using 1×1 convolutions passed the CPU smoke run and timed out during AMD session creation after 75 seconds.

## Split NPU denoiser

The full quantized denoiser works when each learned projection gets its own strict VitisAI session. The CPU runs feature extraction, GRU gates and state updates, and audio reconstruction. AMD's reports assign all ten real convolutions to the NPU. The NPU and CPU versions of the same quantized model both returned 1,312 frames on the test clip, with a 2.87-count RMS difference on signed 16-bit PCM.

```powershell
& $python .\scripts\split-rnnoise-matrices.py .phase1\models\rnnoise_conv_int8.onnx .phase1\models\rnnoise_split
.\scripts\build-phase1.ps1
.\scripts\test-phase1.ps1 -Backend npu -InputPcm .phase1\input.pcm -MatrixDirectory .phase1\models\rnnoise_split
```

The five-minute paced replay produced 29,999 of 29,999 expected output frames. P95 processing was 6.29 ms and P95 completion from the scheduled arrival was 6.71 ms. There were 320 late frames, with a maximum completion delay of 85.23 ms. Completion returned to 5.87 ms on the final frame. The first frame, including session startup with a populated cache, took 4.34 seconds. Logs and the summary are under `.phase1/probe-npu-rnnoise_host-paced.log` and `.phase1/rnnoise-npu-paced-metrics.json`.

A separate synthetic speech fixture mixed with filtered noise and a 120 Hz hum verified noise suppression. After aligning the output by the remaining 480-sample delay, RMS fell by 73.44 dB in the selected noise-only interval. Speech-to-error ratio fell from 13.71 dB to 10.93 dB in the selected speech samples. The NPU produced all 1,512 expected frames and no samples reached the PCM limits. The input, clean reference, and NPU output are in `.phase1/noise-fixture-*.pcm`.

## Windows app

```powershell
.\scripts\build-phase1.ps1 -WinUI
$app = '.\app\winui\bin\Release\NpuBroadcast.exe'
& $app
```

For regular Windows use, open NPU Broadcast and click **Use existing VB-CABLE**. If multiple cables are installed, choose the one you want and click **Name selected cable**. Approve the Windows prompt. If your existing cable is already used for something else, install an [additional VB-CABLE from VB-Audio](https://vb-audio.com/Cable/), such as Cable A. If Windows declines the rename, rename that cable's recording endpoint in **Settings → System → Sound → Input**. An already named VB-CABLE is reused on later launches. VB-CABLE stays listed after NPU Broadcast exits.

Select the named microphone in calling or recording apps. In NPU Broadcast, use the Windows default microphone or select a specific physical input. The experimental native capture driver is also available for [Windows guest testing](driver/README.md).

Switch on **Noise removal**, **Room echo removal**, or both. The desktop pipeline passes through the original voice with both effects off or while the requested model is preparing. With both effects enabled, LocalVQE runs first and RNNoise follows.

The **Voice effects** card shows input and output levels in dBFS and an expandable **Diagnostics** section. **Listen to my processed voice** monitors the outgoing audio through the Windows default playback device. Capture starts when either effect is enabled or another app uses the virtual mic. **Keep microphone active even with effects off** keeps the meters live when no app is using the virtual mic. Settings are saved per Windows user.

Minimize to keep the route available in the notification area. Launching the app again restores the existing window. Closing the main window stops processing and releases the driver connection.

Run `.\scripts\package.ps1 -Version 0.1.2` to build and stage a versioned copy in `releases/v0.1.2`. See [releases](releases/README.md) for the current build. Run `.\scripts\package.ps1` without a version to build `.phase1/NPU-Broadcast-Windows-x64.zip`. Extract the whole ZIP and open `NpuBroadcast.exe`. The WinUI build requires Visual Studio 2022 C++ Build Tools (v143) and Windows SDK 10.0.26100.

The extracted ZIP opened its desktop controls, initialized both recording and playback-loopback endpoints, and ran both NPU effects concurrently with silent headphone output. In the 25-second concurrent test, the noise route inserted 70 ms of silence and discarded 96 samples. The combined route inserted 20 ms of silence. Neither reported a capture gap.

Build the console with `.\scripts\build-phase1.ps1 -App`. Running `.phase1/app-build/Release/npu-broadcast.exe` without arguments prints usage. The console provides `list`, `check <input-index> <output-index>`, `test <input-index> <output-index> <seconds>`, and `run <input-index> <output-index>`. Use `--room-echo` for the LocalVQE effect, `--bypass` for unprocessed audio, and `--loopback` to use a playback device as the source. `test` exercises capture and render with silent output and estimates delay from the capture timestamps and current render padding. The estimate ends at the app's render stream. A recorder on the virtual microphone endpoint measures the full route. The app also reports mean and P95 worker and model time, frames that exceeded their 10 ms or 16 ms processing budget, and the peak depth of each software queue.

The 30-second muted Realtek microphone-to-headphone test processed 3,000 noise-removal frames and inserted 40 ms of silence. The combined effect processed 1,875 hops and inserted 10 ms. Both runs reported zero capture gaps and zero queue overflows. Full end-to-end latency through a virtual microphone remains unmeasured.

The app targets 720 render samples for noise removal and 960 for the combined effect. In two 20-second noise tests from the extracted ZIP, estimated capture-to-render P95 was 79 ms and 60 ms. The first run had two capture gaps and inserted 640 ms of silence. The repeat had zero capture gaps and inserted 30 ms. The combined effect measured 68 ms P95, with zero capture gaps and 10 ms of inserted silence. All three runs reported zero discarded samples. Logs are in `.phase1/packaged-final-*.stdout`.

On this machine, both NPU effects carried synthetic speech from **Steam Streaming Speakers** to a separate recorder opened on **Microphone (Steam Streaming Microphone)**. The noise and combined effects inserted 30 ms and 22 ms of silence respectively while the source was active. The Steam virtual microphone pair showed an approximately 60 ms audible onset difference even with a direct signal. Packet timing was too variable for a repeatable waveform-based end-to-end latency estimate. The under-50 ms target still needs testing with VB-CABLE.

A later probe played eight isolated speech bursts directly into **Speakers (Steam Streaming Microphone)** and recorded **Microphone (Steam Streaming Microphone)** without NPU Broadcast in the path. The onset delay grew from 70 ms to 620 ms over 6.2 seconds. The recording endpoint reports a 44.1 kHz mix while the playback side runs at 48 kHz. This Steam pair cannot provide a stable latency baseline. The same burst probe measures a synthetic 35 ms delay exactly. Fixture and packets are in `.phase1/latency-burst*`.

`scripts/test-virtual-route.ps1` runs the normal application route and captures timestamped source and sink packets for a synthetic PCM fixture. Use its `-Bypass` switch to measure the same route with effects disabled. `scripts/measure-route.py` reports the envelope match and estimates latency only when at least three waveform sections match. Use `list` and `list --loopback` to identify the capture and playback endpoints before invoking the probe. [WASAPI capture timestamps](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiocaptureclient-getbuffer)

Muted tests from the Realtek microphone in bypass mode estimated capture-to-render P95 at 20 ms with Realtek headphones and 21 ms with the Steam virtual playback endpoint. Both recorded a capture gap. On the full Steam virtual route, the bypass probe matched one of 16 waveform sections, below the minimum for a latency estimate. Separate synthetic speech runs over the Steam route measured mean model inference time of 4.24 ms for noise removal and 9.01 ms for the combined effect. Logs are in `.phase1/app-bypass-*.log` and `.phase1/steam-virtual-inference-*.stdout`.

The startup trace showed that Realtek capture delivered two packets before a roughly 350 ms gap as playback came online. Raw microphone mode and waiting for three capture packets before playback both reproduced the gap. For microphone routes, the app now fills the playback buffer with silence, starts playback, and waits up to one second for the render event before preparing the effect and starting capture. Logs for the startup experiments are in `.phase1/capture-gap-*.log`. [WASAPI render startup](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-start)

In 20-second muted Realtek tests with this startup order, bypass, noise removal, and the combined effect all reported zero capture gaps and zero discards. Estimated capture-to-render P95 was 20 ms, 44 ms, and 66 ms respectively. Noise removal inserted 10 ms of silence. Bypass and the combined effect reported zero inserted samples. Logs are in `.phase1/render-ready-*.stdout`.

Two trials starting combined-effect playback after its first 16 ms hop instead of waiting for the 20 ms render target measured 85 ms and 84 ms P95 and inserted silence in both runs. The app uses the 20 ms target. Logs are in `.phase1/render-ready-echo-after-one-hop-*.stdout`.

Supported playback endpoints use WASAPI clock adjustment to raise the stream rate by 1% while processed audio remains queued, then return to 48 kHz as the queue drains. Endpoints without adjustment support continue at the fixed rate. In paired 12-second tests with a forced 30 ms worker pause, second-half capture-to-render P95 fell from 51 ms to 27 ms in bypass, from 75 ms to 48 ms with noise removal, and from 86 ms to 66 ms with both effects. The tests still recorded inserted silence after the pause. The adjusted room-echo run discarded 96 samples and the adjusted bypass run discarded 336 samples. Logs are in `.phase1/render-clock-stall-*`. [WASAPI stream rate](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclockadjustment-setsamplerate)

Without the forced pause, the release build measured 51 ms P95 for noise removal and 67 ms for the combined effect in 20-second muted Realtek runs. Noise removal inserted 20 ms of silence. The combined run recorded one capture gap after 10.05 seconds and inserted 320 ms of silence. Both reported zero discarded samples. The combined effect also passed synthetic speech through the Steam virtual route with no discards. The two signals had insufficient waveform matches for a full-route latency estimate. Logs are in `.phase1/render-clock-release-*.stdout` and `.phase1/steam-virtual-render-clock.stdout`.

A five-minute muted Realtek run of the packaged combined effect started in 6.54 seconds and processed 18,536 hops at 8.27 ms mean model time. Estimated capture-to-render P95 was 74 ms. The app recorded 11 capture gaps, 3.51 seconds of inserted silence, and 2,347 discarded samples. The process used 101.77 CPU seconds, equivalent to 1.41% of the 24 logical processors over the run. See `.phase1/sustain-300s-room-echo.stdout` and `.phase1/sustain-300s-room-echo.metrics.json`.

AMD xrt-smi reported hardware completions increasing from 158,884 to 185,380 for that application PID and an estimated NPU power draw of 0.574 W in one platform snapshot. Three Windows engine samples attributed to the process measured 20.7–21.6% on the adapter that DXCore identified as the AMD NPU (PCI vendor `0x1022`, device `0x17f0`). Counter samples and adapter results are in `.phase1/sustain-300s-room-echo.*.txt`. [AMD NPU management reports](https://ryzenai.docs.amd.com/en/latest/xrt_smi.html) · [DXCore adapter lookup](https://learn.microsoft.com/en-us/windows/win32/dxcore/dxcore_interface/nf-dxcore_interface-idxcoreadapterfactory-getadapterbyluid)

Two 12-second runs with the per-frame metrics measured worker P95 at 5.1 ms for noise removal and 10.6 ms for the combined effect. Each logged one worker frame over budget, one capture gap, and zero queue discards. Capture and render queue peaks were 480 and 960 samples for noise removal, and 1,152 and 1,536 samples for the combined effect. Logs are in `.phase1/frame-timing-*.stdout`.

In matched 75-second muted bypass tests, the Realtek microphone array had zero capture gaps. The UGREEN USB microphone had two waits of 585 ms and 546 ms between packets while the audio thread's service interval stayed below 10.4 ms. Neither run discarded queue samples. The native 48 kHz capture engine minimum was 480 samples (10 ms) for each device. See `.phase1/compare-capture-*.stdout`. [WASAPI shared engine periods](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient3-getsharedmodeengineperiod)

A 90-second muted Realtek run with the combined effect measured two intervals of 317 ms and 281 ms between capture packets. The audio service loop stayed within 10.5 ms during both intervals. Worker P95 was 10.5 ms for the 16 ms hop, and the app discarded zero queue samples. Estimated capture-to-render P95 was 67 ms. `test` reports packet intervals flagged by WASAPI alongside the service intervals and processing budgets. See `.phase1/compare-capture-realtek-active.stdout`. [WASAPI capture flags](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/ne-audioclient-_audclnt_bufferflags)

Three further 90-second muted runs compared Realtek capture modes with the same combined AMD effect. Standard shared capture recorded 2 gaps and 67.4 ms estimated capture-to-render P95. Native four-channel shared capture with app downmix recorded 5 gaps and 90.9 ms. The Windows Speech stream category with mono capture recorded 25 gaps and 83.0 ms. The paired audio service intervals stayed below 10.8 ms in all three runs. Standard shared capture remains the release path. See `.phase1/capture-mode-npu-*.stdout`. [WASAPI native shared streams](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient3-initializesharedaudiostream) · [Windows audio signal processing modes](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/audio-signal-processing-modes)

## Room echo removal

The combined effect uses LocalVQE v1.2 to remove background noise and room reverberation from one stream. It uses a 16 ms hop at 16 kHz. The app resamples between the 48 kHz audio endpoints and the model. The streaming ONNX export matched the CPU batch model across all 504 fixture hops before quantization. [LocalVQE model card](https://huggingface.co/LocalAI-io/LocalVQE)

```powershell
.\scripts\setup-phase1.ps1 -RoomEcho
$python = '.\.phase1\.venv\Scripts\python.exe'
& $python -m pip install onnxruntime==1.24.4
& $python .\scripts\test-room-echo.py .phase1\calibration-speech.wav
& $python .\scripts\export-room-echo.py --quantize
& $python .\scripts\split-rnnoise-matrices.py .phase1\models\room_echo_int8.onnx .phase1\models\room_echo_split --room-echo
.\scripts\build-phase1.ps1
.\scripts\test-room-echo-npu.ps1
& $python .\scripts\test-room-echo.py .phase1\calibration-speech.wav --npu-output .phase1\room-echo-npu.pcm
.\scripts\build-phase1.ps1 -App
```

Use a local 48 kHz mono PCM speech WAV for the test. AMD's assignment reports confirmed all 20 selected encoder and decoder convolutions on the NPU. The NPU path returned all 128,800 output samples. On this synthetic fixture, the speech-to-tail ratio improved from 11.89 dB to 33.49 dB. The CPU reference reached 37.90 dB on the same PCM input, and the NPU speech-region dry-to-error ratio was 4.01 dB. The initial offline run took 13.03 ms P95, with one of 505 hops above 16 ms. Logs and assignment reports are in `.phase1/room-echo-npu.log` and `.phase1/room-echo-vitis-cache/`.

An experiment restored 1,593 tensor annotations in the room-echo ONNX host graph. All six runs returned identical PCM. Three alternating runs per graph gave median per-run P95 of 11.29 ms for the release graph and 11.14 ms with the annotations, with mixed results across the pairs. The release graph is retained. In a separate timing run, the 20 NPU leaf sessions took about 5.40 ms of the 7.84 ms mean model time per hop. The surrounding custom-operator work accounted for about 0.04 ms per hop. This points to the session calls and host graph as the larger costs. See `.phase1/shape-folding-*.log` and `.phase1/leaf-timing-eval.log`. [ONNX Runtime graph optimizations](https://onnxruntime.ai/docs/performance/graph-optimizations/)

Two independent RNNoise projections produced bit-identical results over 256 inputs when grouped into one VitisAI session. AMD's operator report assigned both convolutions to the NPU. Their average grouped call took 0.533 ms, compared with 0.581 ms for the sum of the individual mean times. An adjacent room-echo pair with its intervening normalization and gating failed during strict VitisAI session preparation with `Invalid NodeIndex`. The release uses the verified per-convolution sessions. Logs and assignments are in `.phase1/grouped-projection-probe/`.

## Pass criteria

- **Correct output.** Align input and both outputs, accounting for the demo discarding the first processed frame. Verify intelligible speech, reduced noise during the noise-only interval, and no clipping or silence regressions. Force the AMD NPU to be unavailable once and verify that strict mode fails and produces no CPU-denoised output.
- **Real-time processing.** Pace a five-minute replay at one 480-sample frame every 10 ms. Report startup and first run separately from warm mean and P95 inference time, P95 whole-frame processing time, missed deadlines, queue growth, and process CPU use for each backend. Require evidence of meaningful recurrent computation on the AMD NPU, P95 whole-frame time under 10 ms, a bounded queue, and zero lost frames. Record NPU utilization and dedicated GPU activity where measurable.
- **Next milestone.** After a pass, use WASAPI capture and route through VB-CABLE. Measure added latency through the virtual recording endpoint against the under-50 ms target. If Phase 1 fails, save the graph partition or timing evidence and use it to choose the next model experiment.

VB-CABLE route test. Synthetic speech bursts played to Steam Streaming Speakers, captured through loopback, processed by NPU Broadcast, sent to CABLE Input, and recorded from CABLE Output. scripts/measure-bursts.py measures burst onset delay. Median and maximum delays were 75 and 80 ms with effects off, 105 and 105 ms with noise removal, and 135 and 225 ms with noise plus room echo removal. Noise removal added about 30 ms over the same route with effects off. The combined effect added about 60 ms median and 145 ms at worst, which misses the 50 ms target. Each run recorded one capture gap. The effects-off figure includes loopback capture of the source endpoint, so it is an upper bound for the app's own buffering. Packets are in .phase1/vbcable-route-*.

VB-CABLE latency, measured by aligning the speech envelope of eight synthetic bursts (scripts/measure-bursts.py). Source is loopback of Steam Streaming Speakers and sink is CABLE Output. A direct write to CABLE Input measured -40 ms against its own loopback, so the loopback timestamps carry an offset and only differences between modes are reliable. With effects off the route measured 75 ms median. Noise removal measured 102 and 112 ms in two runs, adding about 27 to 37 ms. Noise plus room echo removal measured 127, 131, and 114 ms, adding about 39 to 56 ms, with a worst burst of 138 ms. After slow frames the app now drops near-silent samples below -54 dBFS from the front of the render queue whenever it holds more than two model hops. Each run still logged one or two capture gaps. Packets are in .phase1/vbcable-*.

Clock-referenced VB-CABLE latency. route-probe now writes the playback device clock (IAudioClock positions) to *.sink.clock, and scripts/measure-bursts.py --fixture places each played burst on that clock instead of on loopback timestamps. A direct write into CABLE Input measured 73 ms median from the playback clock to CABLE Output. With NPU Broadcast between Steam Streaming Speakers loopback and CABLE Input, effects off measured 86 ms, noise removal 135 and 128 ms, and noise plus room echo removal 136 and 153 ms. Subtracting the 73 ms baseline route, the app added about 13 ms with effects off, 55 to 62 ms with noise removal, and 63 to 80 ms with both effects. This misses the 50 ms target. These totals include two driver buffers outside the app (the Steam loopback source and VB-CABLE), so the app share is an estimate. Every run logged one capture gap. Packets and clocks are in .phase1/vbcable-clock-*.

Paired VB-CABLE run with in-app timing enabled outside test mode. Effects off measured 101 ms on the playback clock and noise removal 137 ms, so noise removal added 36 ms in this pair. The app's own capture-to-render P95 for noise removal was 13.5 ms. Worker P95 was 4.6 ms with no frames over budget and render queue peaks of 960 samples. RNNoise also needs 480 samples of analysis lookahead, which accounts for about 10 ms of algorithmic delay. The effects-off baseline varied from 86 to 101 ms across runs, so run-to-run spread is about 15 ms. Logs are in .phase1/vbcable-internal-*.

Three alternating VB-CABLE pairs measured effects off at 116, 98, and 95 ms and noise removal at 137.5, 138, and 137 ms median. Noise removal therefore added 21.5, 40, and 42 ms per pair, with a mean of 34.5 ms. The worst noise burst reached 157 ms against a 95 to 122 ms effects-off range. Every run logged one or two capture gaps. Logs are in .phase1/vbcable-pair*.

Three alternating VB-CABLE pairs for noise plus room echo removal. Effects off measured 100.5, 120, and 97 ms. The combined effect measured 134, 150, and 162 ms median. It added 33.5, 30, and 65 ms, for a mean of 42.8 ms. One of the three pairs missed the 50 ms target. In the first combined run, the app's own capture-to-render P95 was 37.2 ms. Worker P95 was 12.8 ms against the 16 ms hop. Seven hops ran over budget, and the render queue peaked at 1,536 samples. Every run logged one capture gap. Logs are in .phase1/vbcable-echo-*.

Smaller room-echo render target experiment. Lowering the combined effect's render target from 960 to 720 samples measured effects off at 100, 107.5, and 101 ms and the combined effect at 137.5, 161, and 154 ms median, adding 37.5, 53.5, and 53 ms. It also inserted more silence, up to 3,504 samples per run. The 960-sample target averaged 42.8 ms added and is retained. The combined effect has about 16 ms of hop accumulation, 16 ms of LocalVQE overlap-add, and about 2.6 ms of 48-to-16 kHz resampling. That leaves under 16 ms of headroom below 50 ms for render buffering and worker jitter. Its worker P95 of 12.8 ms is close to the 16 ms hop. Logs are in .phase1/vbcable-echo720-*.

Pro Audio thread priority experiment. Registering the audio threads as MMCSS Pro Audio at high priority measured effects off at 101.5, 102, and 106 ms and the combined effect at 148, 151, and 169 ms, adding 46.5, 49, and 63 ms. Worker P95 was 11.1 to 12.8 ms, with 4 to 8 hops over budget, so the priority change did not reduce combined-effect delay. The app keeps the standard Audio class. Logs are in .phase1/vbcable-proaudio-*.

Room-echo delay breakdown. The offline AMD NPU tool removes the one-hop output delay, so its burst alignment is 0 ms. That excludes the 16 ms hop accumulation present in the live app. On the clock-referenced VB-CABLE runs, render-to-loopback measured 26.6 to 34.5 ms in all modes. The app's own capture-to-render P95 was 29.4 to 39.3 ms for the combined effect. That includes the 16 ms hop and 16 ms LocalVQE overlap-add. The remaining combined-effect variation comes from worker overruns, 2 to 8 per run.

Earlier low-delay room-echo experiment. This version emitted the newest half of each 32 ms LocalVQE window directly, skipping one 16 ms overlap-add hop. RNNOISE_WINML_ECHO_OVERLAP_ADD=1 restores the previous output. On the offline NPU fixture, after aligning for the removed hop, the speech-to-tail ratio was 31.49 dB with this output and 33.49 dB with overlap-add. The speech-region signal-to-dry-error ratio was 2.91 dB and 4.01 dB respectively. Offline P95 per hop was 11.82 ms with this output and 13.07 ms with overlap-add. Six alternating VB-CABLE pairs measured effects off at 111, 92, 119, 93, 100, and 113 ms. The combined effect measured 121, 141, 147.5, 134, 123.5, and 127 ms, adding 10, 49, 28.5, 41, 23.5, and 14 ms. The mean added delay was 27.7 ms, and every pair stayed under 50 ms. Logs are in .phase1/vbcable-echolow-*.

The desktop app now requests overlap-add reconstruction for room echo. It combines adjacent windows to reduce speech artifacts, adding one 16 ms hop compared with the low-delay experiment above. The strength control blends the processed signal with time-aligned original audio and ramps changes over one hop. The six low-delay route measurements above apply to the earlier reconstruction.
