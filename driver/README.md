# NPU Broadcast virtual microphone

The desktop app uses a capture-only SysVAD driver. An exclusive producer handle
controls jack presence. A WaveRT capture stream consumes signed 16-bit 48 kHz
mono audio. The driver counts underruns and dropped samples. The prototype has
one kernel capture pin instance. Verify that Windows shares it between the
target recording apps in the guest.

## Build and stage

Use VS 2022 x64 C++ Build Tools, Windows SDK 10.0.26100.0, Python, CMake, and an
extracted Microsoft.Windows.WDK.x64 10.0.26100.6584 NuGet package. Pass its
`c` directory as the kit root. The build script compiles and links the unsigned
driver, stamps the INF, and runs InfVerif.

```powershell
git clone --filter=blob:none --no-checkout https://github.com/microsoft/Windows-driver-samples.git .phase1/sysvad-upstream
git -C .phase1/sysvad-upstream sparse-checkout set audio/sysvad
git -C .phase1/sysvad-upstream checkout --detach d49bb7d72f3651952b2b6e4ad28d14fe2247eb69
.\scripts\build-mic-driver.ps1 -WdkRoot .phase1\wdk-26100\c
cmake -S driver -B .phase1/npu-mic-probe -G 'Visual Studio 17 2022' -A x64
cmake --build .phase1/npu-mic-probe --config Release
```

Copy the files from `.phase1/npu-mic-kernel/unsigned` and
`.phase1/npu-mic-probe/Release/npu-mic-probe.exe` to the guest.

## Windows 11 x64 test guest

Use a disposable guest configured for Microsoft test signing. In the guest's
WDK environment, sign the SYS with the test certificate, use Inf2Cat to
generate `NpuBroadcastMic.cat` from the staged INF and signed SYS, then sign
the catalog. Trust the test certificate in the guest. Run the following in an elevated terminal
inside the guest once both signatures are in place.

```powershell
pnputil /add-driver .\NpuBroadcastMic.inf
devgen /add /bus ROOT /hardwareid Root\NpuBroadcastMic
```

Record the instance ID printed by DevGen. Verify the node is started using
`pnputil /enum-devices /instanceid <instance-id>`. Remove it with
`devgen /remove <instance-id>` after the test.

1. Run `npu-mic-probe.exe --endpoint` before opening the producer. Exit code 1
   means Windows has no active recording endpoint with the exact branded name.
2. Run `npu-mic-probe.exe` in one terminal. From a second terminal, run the
   endpoint check and save the printed ID. Select **Microphone (NPU Broadcast)**
   in OBS and verify a 500 Hz tone. Minimize the producer and keep recording.
3. Force-quit the producer. Repeat the endpoint check until Windows reports it
   inactive. Restart the producer and confirm the ID matches and OBS can use
   its saved selection. Repeat closing normally and through the tray.
4. Close the tone producer and run NPU Broadcast. With an active OBS recording,
   toggle noise and echo removal through all combinations. Check for missing
   samples, dry fallback during model startup, and uninterrupted device
   attachment. Repeat with two shared-mode consumers and with monitoring on.
   Also verify sleep, resume, physical microphone reconnection, and app exit.

The desktop maintains a 40 ms dry buffer so each output sample can use the
processed sample for the same position or pass through the original sample.
Measure added latency and the frequency of dry fallback on the target NPU.
Public release requires HLK testing and Microsoft dashboard signing. Ship the
signed INF, SYS, and catalog through the production installer after the guest
checks pass.
