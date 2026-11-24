from pathlib import Path
import io
import re
import shutil
import subprocess
import sys
import zipfile

REVISION = "d49bb7d72f3651952b2b6e4ad28d14fe2247eb69"
checkout = Path(sys.argv[1]).resolve()
if subprocess.check_output(["git", "-C", str(checkout), "rev-parse", "HEAD"], text=True).strip() != REVISION:
    raise SystemExit(f"Expected Windows-driver-samples commit {REVISION}")

source = Path(__file__).resolve().parent
destination = Path(sys.argv[2]).resolve()
if destination == checkout or checkout in destination.parents or destination in checkout.parents:
    raise SystemExit("Use a separate generated source directory outside the vendor checkout.")
archive = subprocess.check_output(["git", "-C", str(checkout), "archive", "--format=zip", "HEAD", "audio/sysvad", "LICENSE"])
with zipfile.ZipFile(io.BytesIO(archive)) as package:
    package.extractall(destination)
sysvad = destination / "audio/sysvad"
tablet = "TabletAudioSample/"
endpoints = "EndpointsCommon/"


def upstream(name):
    return subprocess.check_output(
        ["git", "-C", str(checkout), "show", f"HEAD:audio/sysvad/{name}"]
    ).decode("utf-8-sig").replace("\r\n", "\n")


def change(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"Expected a single match for {old!r}")
    return text.replace(old, new)


def write(name, text):
    (sysvad / name).write_text(text, encoding="utf-8")


for name in ("producer.cpp", "producer.h", "protocol.h"):
    shutil.copyfile(source / name, sysvad / name)

name = "adapter.cpp"
text = upstream(name)
text = change(text, "    DriverObject->MajorFunction[IRP_MJ_PNP] = PnpHandler;",
              """    DriverObject->MajorFunction[IRP_MJ_PNP] = PnpHandler;
    NpuMicInitialize();
    NpuMicAttachDispatch(DriverObject);""")
text = change(text, "    ntStatus = InstallAllRenderFilters(DeviceObject, Irp, pAdapterCommon);\n    IF_FAILED_JUMP(ntStatus, Exit);",
              "    if (false) InstallAllRenderFilters(DeviceObject, Irp, pAdapterCommon);")
for feature in ("SYSVAD_BTH_BYPASS", "SYSVAD_USB_SIDEBAND", "SYSVAD_A2DP_SIDEBAND"):
    text = re.sub(rf"#ifdef {feature}\n    if \(!g_Disable[^\n]+\n.*?#endif[^\n]*\n",
                  "", text, count=1, flags=re.S)
text = change(text, "Exit:\n\n    //\n    // Stash the adapter", """    ntStatus = NpuMicCreateControl(DeviceObject->DriverObject);

Exit:

    //
    // Stash the adapter""")
text = change(text, "    case IRP_MN_STOP_DEVICE:\n        ext =", "    case IRP_MN_STOP_DEVICE:\n        NpuMicDeleteControl();\n        ext =")
write(name, change(text, "#include <sysvad.h>", '#include <sysvad.h>\n#include "producer.h"'))

name = "basetopo.cpp"
text = upstream(name)
text = text.replace("    SAFE_RELEASE(m_PortEvents);", "    if (m_PortEvents) NpuMicSetPortEvents(nullptr);\n    SAFE_RELEASE(m_PortEvents);", 1)
text = change(text, "        m_AdapterCommon->MixerReset();",
              "        m_AdapterCommon->MixerReset();\n        NpuMicSetPortEvents(m_PortEvents);")
write(name, change(text, "#include <sysvad.h>", '#include <sysvad.h>\n#include "producer.h"'))

name = tablet + "micintopo.cpp"
text = upstream(name)
text = change(text, "                        RtlCopyMemory(pDesc, JackDescriptions[nPinId], sizeof(KSJACK_DESCRIPTION));",
    "                        RtlCopyMemory(pDesc, JackDescriptions[nPinId], sizeof(KSJACK_DESCRIPTION));\n                        pDesc->IsConnected = NpuMicConnected();")
text = change(text, "pDesc->JackCapabilities = 0;", "pDesc->JackCapabilities = JACKDESC2_PRESENCE_DETECT_CAPABILITY;")
write(name, change(text, "#include <sysvad.h>", '#include <sysvad.h>\n#include "producer.h"'))

name = tablet + "micintoptable.h"
text = upstream(name)
text = change(text, "DEFINE_PCAUTOMATION_TABLE_PROP(AutomationMicInTopoFilter, MicInPropertiesTopoFilter);", """
static PCEVENT_ITEM MicInJackEvents[] = {
    { &KSEVENTSETID_PinCapsChange, KSEVENT_PINCAPS_JACKINFOCHANGE,
      KSEVENT_TYPE_ENABLE | KSEVENT_TYPE_BASICSUPPORT, CMiniportTopology_EventHandler_JackState }
};
DEFINE_PCAUTOMATION_TABLE_PROP_EVENT(AutomationMicInTopoFilter, MicInPropertiesTopoFilter, MicInJackEvents);
""")
text = change(text, "&MICIN_CUSTOM_NAME,", "NULL,")
write(name, text)

name = tablet + "minipairs.h"
text = upstream(name)
text = re.sub(r"(g_CaptureEndpoints\[\]\s*=\s*\{)[^}]+", r"\1\n    &MicInMiniports,\n", text, count=1)
write(name, text)

name = tablet + "micinwavtable.h"
text = upstream(name)
start = text.index("KSDATAFORMAT_WAVEFORMATEXTENSIBLE MicInPinSupportedDeviceFormats[]")
end = text.index("\n};", start)
# Retain only the upstream 48 kHz PCM16 entry. Shared WASAPI handles client resampling.
entries = re.split(r"\n    \},", text[start:end])
entry = next(part for part in entries if re.search(r"\b48000,\s*\n\s*96000,\s*\n\s*2,\s*\n\s*16,", part))
entry = entry.split("{", 1)[-1] if entry.startswith("KSDATAFORMAT") else entry
entry = entry.rstrip().removesuffix(",")
if not entry.rstrip().endswith("\n    }"):
    entry += "\n    }"
text = text[:start] + "KSDATAFORMAT_WAVEFORMATEXTENSIBLE MicInPinSupportedDeviceFormats[] =\n{" + entry + text[end:]
text = re.sub(r"&MicInPinSupportedDeviceFormats\[\d+\]\.DataFormat,.*",
              "&MicInPinSupportedDeviceFormats[0].DataFormat,", text)
text = change(text, "#define MICIN_MAX_INPUT_STREAMS             5", "#define MICIN_MAX_INPUT_STREAMS             1")
text = re.sub(r"(#define MICIN_(?:MIN|MAX)_SAMPLE_RATE\s+)\d+", r"\g<1>48000", text)
write(name, text)

name = endpoints + "minwavertstream.cpp"
text = upstream(name)
start = text.index("CMiniportWaveRTStream::~CMiniportWaveRTStream")
brace = text.index("{", start)
text = text[:brace+1] + "\n    if (m_KsState == KSSTATE_RUN && m_bCapture) NpuMicStreamState(false);" + text[brace+1:]
text = change(text, "    m_KsState = State_;", """    if (m_bCapture && (m_KsState == KSSTATE_RUN) != (State_ == KSSTATE_RUN)) {
        NpuMicStreamState(State_ == KSSTATE_RUN);
    }
    m_KsState = State_;""")
text = change(text, "            m_ToneGenerator.GenerateSine(m_pDmaBuffer + bufferOffset, runWrite);",
              "        NpuMicRead(m_pDmaBuffer + bufferOffset, runWrite);")
write(name, change(text, "#include <sysvad.h>", '#include <sysvad.h>\n#include "producer.h"'))

for name in (tablet + "TabletAudioSample.vcxproj", endpoints + "EndpointsCommon.vcxproj"):
    text = upstream(name)
    if name.startswith(tablet):
        text = change(text, '<ClCompile Include="..\\adapter.cpp" />',
                      '<ClCompile Include="..\\adapter.cpp" />\n    <ClCompile Include="..\\producer.cpp" />')
        text = text.replace("portcls.lib;", "portcls.lib;wdmsec.lib;")
    write(name, text)

for inf in (sysvad / tablet).glob("*.inx"):
    if inf.name != "NpuBroadcastMic.inx":
        inf.unlink()
shutil.copyfile(source / "NpuBroadcastMic.inx", sysvad / tablet / "NpuBroadcastMic.inx")

