import argparse
import struct
from pathlib import Path

import numpy as np


RATE = 48_000
TICKS_PER_SECOND = 10_000_000
HEADER = struct.Struct("<QII")
TIMESTAMP_ERROR = 4


def packets(path):
    frames = []
    rejected = 0
    with path.open("rb") as stream:
        while header := stream.read(HEADER.size):
            if len(header) != HEADER.size:
                raise ValueError(f"Truncated packet header in {path}.")
            timestamp, size, flags = HEADER.unpack(header)
            if size > RATE:
                raise ValueError(f"Unexpected WASAPI packet size in {path}.")
            data = stream.read(size * 4)
            if len(data) != size * 4:
                raise ValueError(f"Truncated packet audio in {path}.")
            if not timestamp or flags & TIMESTAMP_ERROR:
                rejected += 1
                continue
            frames.append((timestamp, np.frombuffer(data, dtype="<f4")))
    if not frames:
        raise ValueError(f"No timestamped audio in {path}.")
    return frames, rejected


def timeline(frames, origin):
    chunks = [(round((timestamp - origin) * RATE / TICKS_PER_SECOND), audio) for timestamp, audio in frames]
    length = max(offset + audio.size for offset, audio in chunks)
    if min(offset for offset, _ in chunks) < 0:
        raise ValueError("Probe timestamps moved backwards past the first packet.")
    if length > RATE * 30:
        raise ValueError("Probe timestamps span more than 30 seconds.")
    samples = np.zeros(length, dtype=np.float32)
    for offset, audio in chunks:
        samples[offset:offset + audio.size] = audio
    return np.nan_to_num(samples, copy=False)


def delays(source, sink):
    window = RATE // 4
    min_offset = -RATE // 100
    max_offset = RATE // 4
    block_energy = [np.mean(source[start:start + window].astype(np.float64) ** 2)
                    for start in range(0, source.size - window, window)]
    if not block_energy:
        return [], 0
    floor = max(max(block_energy) * 0.03, 1e-7)
    matches = []
    considered = 0
    for index, energy in enumerate(block_energy):
        start = index * window
        if energy < floor or start + min_offset < 0 or start + window + max_offset > sink.size:
            continue
        considered += 1
        reference = source[start:start + window].astype(np.float64)
        target = sink[start + min_offset:start + window + max_offset].astype(np.float64)
        nfft = 1 << (reference.size + target.size - 2).bit_length()
        convolution = np.fft.irfft(np.fft.rfft(reference[::-1], nfft) * np.fft.rfft(target, nfft), nfft)
        numerator = convolution[window - 1:target.size]
        integral = np.concatenate(([0.0], np.cumsum(target * target)))
        power = integral[window:] - integral[:-window]
        norm = np.sqrt(np.sum(reference * reference) * np.maximum(power, 0.0))
        correlations = np.divide(numerator, norm, out=np.zeros_like(numerator), where=norm > 1e-9)
        best = int(np.argmax(correlations))
        if correlations[best] >= 0.35:
            matches.append(((min_offset + best) * 1000 / RATE, float(correlations[best])))
    return matches, considered


def envelope_match(source, sink):
    block = RATE // 200
    envelopes = [np.sqrt(np.mean(signal[:signal.size // block * block].reshape(-1, block) ** 2, axis=1))
                 for signal in (source, sink)]
    a, b = envelopes
    if not a.size or not b.size:
        return 0.0, None
    nfft = 1 << (a.size + b.size - 2).bit_length()
    score = np.fft.irfft(np.fft.rfft(b, nfft) * np.conj(np.fft.rfft(a, nfft)), nfft)
    lag = int(np.argmax(score))
    normalizer = np.linalg.norm(a) * np.linalg.norm(b)
    coefficient = float(score[lag] / normalizer) if normalizer else 0.0
    onsets = [np.flatnonzero(values >= 0.01) for values in envelopes]
    onset = 5 * int(onsets[1][0] - onsets[0][0]) if all(len(value) for value in onsets) else None
    return coefficient, onset


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("result_base", type=Path)
    args = parser.parse_args()
    try:
        source_frames, source_bad = packets(Path(str(args.result_base) + ".source.packets"))
        sink_frames, sink_bad = packets(Path(str(args.result_base) + ".sink.packets"))
        origin = min(source_frames[0][0], sink_frames[0][0])
        source = timeline(source_frames, origin)
        sink = timeline(sink_frames, origin)
        print(f"Source peak {np.max(np.abs(source)):.3f}. Sink peak {np.max(np.abs(sink)):.3f}.")
        print(f"Discarded packets with unreliable timestamps: {source_bad} source, {sink_bad} sink.")
        coefficient, onset = envelope_match(source, sink)
        print(f"Speech envelope correlation {coefficient:.2f}.")
        if onset is not None:
            print(f"First audible onset difference {onset} ms (5 ms bins).")
        matches, considered = delays(source, sink)
        if len(matches) < 3:
            raise ValueError(f"Only {len(matches)}/{considered} sections matched. No latency estimate.")
        measurements = np.array(matches)
        print(f"{len(matches)}/{considered} speech sections matched. Median correlation {np.median(measurements[:, 1]):.2f}.")
        print(f"Route latency median {np.median(measurements[:, 0]):.2f} ms, "
              f"P95 {np.percentile(measurements[:, 0], 95):.2f} ms.")
    except ValueError as error:
        parser.exit(1, f"Audio route probe: {error}\n")


if __name__ == "__main__":
    main()
