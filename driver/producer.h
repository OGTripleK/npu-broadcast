#pragma once
#include "protocol.h"

void NpuMicInitialize();
void NpuMicAttachDispatch(PDRIVER_OBJECT driver);
NTSTATUS NpuMicCreateControl(PDRIVER_OBJECT driver);
void NpuMicDeleteControl();
void NpuMicSetPortEvents(PPORTEVENTS events);
bool NpuMicConnected();
void NpuMicStreamState(bool running);
void NpuMicRead(BYTE *buffer, ULONG bytes);
