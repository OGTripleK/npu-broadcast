#pragma once

#define NPU_MIC_PATH L"\\\\.\\NpuBroadcastMic"
#define NPU_MIC_ENDPOINT L"Microphone (NPU Broadcast)"
#define NPU_MIC_DEVICE_TYPE 0x8000
#define NPU_MIC_RATE 48000
#define NPU_MIC_PACKET_SAMPLES 480
#define IOCTL_NPU_MIC_WRITE CTL_CODE(NPU_MIC_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_NPU_MIC_STATUS CTL_CODE(NPU_MIC_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_READ_DATA)

struct NpuMicStatus {
    unsigned long capture_streams;
    unsigned long underrun_samples;
    unsigned long dropped_samples;
};
