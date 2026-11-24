import argparse
import runpy
from pathlib import Path

import numpy as np

BLOCK = 240


def bursts(signal):
    frames = signal[:signal.size // BLOCK * BLOCK].reshape(-1, BLOCK)
    rms = np.sqrt(np.mean(frames * frames, axis=1))
    audible = np.flatnonzero(rms >= max(0.005, np.max(rms) * 0.12))
    if not audible.size:
        return []
    groups = np.split(audible, np.flatnonzero(np.diff(audible) > 10) + 1)
    return [group[0] * BLOCK for group in groups if len(group) >= 5]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("result_base", type=Path)
    parser.add_argument("--fixture", type=Path, help="Measure from the render clock instead of loopback")
    args = parser.parse_args()
    probe = runpy.run_path(str(Path(__file__).with_name("measure-route.py")))
    source_frames, _ = probe["packets"](Path(str(args.result_base) + ".source.packets"))
    sink_frames, _ = probe["packets"](Path(str(args.result_base) + ".sink.packets"))
    origin = min(source_frames[0][0], sink_frames[0][0])
    source = probe["timeline"](source_frames, origin)
    sink = probe["timeline"](sink_frames, origin)
    if args.fixture:
        # Place the played fixture on the render device clock and use it as the reference.
        clock = np.fromfile(str(args.result_base) + ".sink.clock", "<u8")
        frequency, lead_in = int(clock[0]), int(clock[1])
        pairs = clock[2:].reshape(-1, 2).astype(np.float64)
        frames = pairs[:, 0] * 48000 / frequency
        # Interpolate the device clock so playback stalls do not shift later bursts.
        def sample_time(index):
            return round((np.interp(lead_in + index, frames, pairs[:, 1]) - origin) * 48000 / 10_000_000)
        played = np.fromfile(args.fixture, "<i2").astype(np.float32) / 32768
        source = np.zeros(sink.size + 48000, np.float32)
        for onset in bursts(played):
            at = sample_time(onset)
            clip = played[onset:onset + 9600]
            source[at:at + clip.size] = clip[:max(0, source.size - at)]
    source_onsets = bursts(source)
    if len(source_onsets) != 8:
        raise SystemExit(f"Expected 8 source bursts, found {len(source_onsets)}.")
    # Align each burst by its 1 ms envelope so attenuated onsets do not bias the result.
    step = 48
    envelope = lambda signal: np.sqrt(np.convolve(signal * signal, np.ones(240) / 240, "same"))[::step]
    source_env, sink_env = envelope(source), envelope(sink)
    delays = []
    for onset in source_onsets:
        start = onset // step
        reference = source_env[start:start + 1800]
        scores = [np.dot(reference, sink_env[start + lag:start + lag + reference.size]) /
                  (np.linalg.norm(sink_env[start + lag:start + lag + reference.size]) + 1e-9)
                  for lag in range(-50, 400) if start + lag >= 0 and start + lag + reference.size <= sink_env.size]
        if not scores:
            raise SystemExit("The sink recording ended before a burst.")
        delays.append(float(np.argmax(scores) - 50))
    delays = np.array(delays)
    print("Burst delays ms:", " ".join(f"{delay:.0f}" for delay in delays))
    print(f"Median {np.median(delays):.1f} ms, maximum {np.max(delays):.1f} ms, drift {delays[-1] - delays[0]:.1f} ms")


if __name__ == "__main__":
    main()
