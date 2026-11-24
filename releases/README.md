# NPU Broadcast releases

**Current release: v0.1.3.** Open `v0.1.3\NpuBroadcast.exe` and keep the files
in that folder together. Enabling either effect starts microphone capture.
Close any running copy before opening this release.

Development builds live under `.phase1` and `app/winui/bin`. Create the
next release with `.\scripts\package.ps1 -Version 0.1.4`. To publish an
already verified build, also pass `-BuildPath <directory>`. The packaging
command refuses to overwrite a release version.
