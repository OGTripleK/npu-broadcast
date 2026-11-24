#include <sysvad.h>
#include <wdmsec.h>
#include "producer.h"

namespace {
constexpr ULONG capacity = NPU_MIC_PACKET_SAMPLES * 6;
KSPIN_LOCK pcm_lock;
KMUTEX jack_lock;
PDEVICE_OBJECT control;
PDRIVER_DISPATCH previous_dispatch[IRP_MJ_MAXIMUM_FUNCTION + 1];
const GUID device_class = {0x9aa90342, 0xfb7e, 0x46c1, {0x86, 0x9c, 0xf7, 0x2b, 0xad, 0x0c, 0x24, 0x62}};
UNICODE_STRING link = RTL_CONSTANT_STRING(L"\\DosDevices\\Global\\NpuBroadcastMic");
PPORTEVENTS port_events;
volatile LONG connected;
volatile LONG running_streams;
short pcm[capacity];
ULONG head, used, underrun, dropped;
}

void NpuMicInitialize() {
    KeInitializeSpinLock(&pcm_lock);
    KeInitializeMutex(&jack_lock, 0);
}

void NpuMicSetPortEvents(PPORTEVENTS events) {
    KeWaitForSingleObject(&jack_lock, Executive, KernelMode, FALSE, nullptr);
    port_events = events;
    KeReleaseMutex(&jack_lock, FALSE);
}

bool NpuMicConnected() {
    return InterlockedCompareExchange(&connected, 0, 0) != 0;
}

static void ChangePresence(bool present) {
    KIRQL previous;
    KeAcquireSpinLock(&pcm_lock, &previous);
    head = used = 0;
    if (present) underrun = dropped = 0;
    const LONG old = InterlockedExchange(&connected, present ? 1 : 0);
    KeReleaseSpinLock(&pcm_lock, previous);

    // PortCls owns the event interface. Keep its release ordered with notifications.
    KeWaitForSingleObject(&jack_lock, Executive, KernelMode, FALSE, nullptr);
    if (port_events && old != (present ? 1 : 0)) {
        port_events->GenerateEventList(
            const_cast<GUID *>(&KSEVENTSETID_PinCapsChange),
            KSEVENT_PINCAPS_JACKINFOCHANGE, FALSE, 0, FALSE, 0);
    }
    KeReleaseMutex(&jack_lock, FALSE);
}

void NpuMicStreamState(bool running) {
    KIRQL previous;
    KeAcquireSpinLock(&pcm_lock, &previous);
    if (running) {
        InterlockedIncrement(&running_streams);
    } else if (InterlockedDecrement(&running_streams) == 0) {
        head = used = 0;
    }
    KeReleaseSpinLock(&pcm_lock, previous);
}

void NpuMicRead(BYTE *buffer, ULONG bytes) {
    const ULONG count = bytes / sizeof(short);
    auto samples = reinterpret_cast<short *>(buffer);
    KIRQL previous;
    KeAcquireSpinLock(&pcm_lock, &previous);
    const ULONG available = min(count, used);
    const ULONG first = min(available, capacity - head);
    RtlCopyMemory(samples, pcm + head, first * sizeof(short));
    RtlCopyMemory(samples + first, pcm, (available - first) * sizeof(short));
    head = (head + available) % capacity;
    used -= available;
    underrun += count - available;
    KeReleaseSpinLock(&pcm_lock, previous);
    RtlZeroMemory(buffer + available * sizeof(short), bytes - available * sizeof(short));
}

static NTSTATUS ProducerIo(PDEVICE_OBJECT device, PIRP irp) {
    const auto stack = IoGetCurrentIrpStackLocation(irp);
    if (device->DeviceType != NPU_MIC_DEVICE_TYPE) {
        return previous_dispatch[stack->MajorFunction](device, irp);
    }
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR written = 0;
    switch (stack->MajorFunction) {
    case IRP_MJ_CREATE:
        if (stack->FileObject->FileName.Length) {
            result = STATUS_OBJECT_NAME_NOT_FOUND;
            break;
        }
        ChangePresence(true);
        result = STATUS_SUCCESS;
        break;
    case IRP_MJ_CLEANUP:
        ChangePresence(false);
        result = STATUS_SUCCESS;
        break;
    case IRP_MJ_CLOSE:
        result = STATUS_SUCCESS;
        break;
    case IRP_MJ_DEVICE_CONTROL: {
        const auto lengths = stack->Parameters.DeviceIoControl;
        void *buffer = irp->AssociatedIrp.SystemBuffer;
        if (lengths.IoControlCode == IOCTL_NPU_MIC_WRITE) {
            const ULONG bytes = lengths.InputBufferLength;
            if (!bytes || bytes > NPU_MIC_PACKET_SAMPLES * sizeof(short) || bytes % sizeof(short)) {
                result = STATUS_INVALID_BUFFER_SIZE;
                break;
            }
            const ULONG count = bytes / sizeof(short);
            const auto samples = static_cast<const short *>(buffer);
            KIRQL previous;
            KeAcquireSpinLock(&pcm_lock, &previous);
            if (connected && running_streams > 0) {
                const ULONG overflow = used + count > capacity ? used + count - capacity : 0;
                head = (head + overflow) % capacity;
                used -= overflow;
                dropped += overflow;
                const ULONG tail = (head + used) % capacity;
                const ULONG first = min(count, capacity - tail);
                RtlCopyMemory(pcm + tail, samples, first * sizeof(short));
                RtlCopyMemory(pcm, samples + first, (count - first) * sizeof(short));
                used += count;
            }
            KeReleaseSpinLock(&pcm_lock, previous);
            result = STATUS_SUCCESS;
        } else if (lengths.IoControlCode == IOCTL_NPU_MIC_STATUS) {
            if (lengths.OutputBufferLength < sizeof(NpuMicStatus)) {
                result = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            auto status = static_cast<NpuMicStatus *>(buffer);
            KIRQL previous;
            KeAcquireSpinLock(&pcm_lock, &previous);
            status->capture_streams = static_cast<ULONG>(running_streams);
            status->underrun_samples = underrun;
            status->dropped_samples = dropped;
            KeReleaseSpinLock(&pcm_lock, previous);
            written = sizeof(*status);
            result = STATUS_SUCCESS;
        }
        break;
    }
    }
    irp->IoStatus.Status = result;
    irp->IoStatus.Information = written;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return result;
}

void NpuMicAttachDispatch(PDRIVER_OBJECT driver) {
    const UCHAR majors[] = {IRP_MJ_CREATE, IRP_MJ_CLEANUP, IRP_MJ_CLOSE, IRP_MJ_DEVICE_CONTROL};
    for (const auto major : majors) {
        previous_dispatch[major] = driver->MajorFunction[major];
        driver->MajorFunction[major] = ProducerIo;
    }
}

NTSTATUS NpuMicCreateControl(PDRIVER_OBJECT driver) {
    if (control) return STATUS_DEVICE_BUSY;
    UNICODE_STRING security = RTL_CONSTANT_STRING(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)");
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\Device\\NpuBroadcastMic");
    auto result = IoCreateDeviceSecure(driver, 0, &name, NPU_MIC_DEVICE_TYPE,
        FILE_DEVICE_SECURE_OPEN, TRUE, &security, &device_class, &control);
    if (!NT_SUCCESS(result)) return result;
    result = IoCreateSymbolicLink(&link, &name);
    if (NT_SUCCESS(result)) {
        control->Flags &= ~DO_DEVICE_INITIALIZING;
    } else {
        IoDeleteDevice(control);
        control = nullptr;
    }
    return result;
}

void NpuMicDeleteControl() {
    if (!control) return;
    IoDeleteSymbolicLink(&link);
    ChangePresence(false);
    IoDeleteDevice(control);
    control = nullptr;
}
