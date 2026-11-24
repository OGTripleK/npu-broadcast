import argparse
import wave
from pathlib import Path

import numpy as np
import torch
from localvqe_reference import load_localvqe


RATE = 16000


def read_speech(path):
    with wave.open(str(path), "rb") as file:
        if (file.getnchannels(), file.getsampwidth(), file.getframerate()) != (1, 2, 48000):
            raise ValueError("Provide a 48 kHz mono 16-bit PCM WAV.")
        raw = np.frombuffer(file.readframes(min(file.getnframes(), 48000 * 8)), "<i2")
    if len(raw) < 48000:
        raise ValueError("Provide at least one second of speech.")
    taps = np.arange(-31, 32)
    filter_taps = np.sinc(taps / 3) * np.hamming(taps.size)
    base = np.convolve(raw.astype(np.float32) / 32768, filter_taps / filter_taps.sum(), "same")[::3]
    block_size = RATE // 20
    blocks = base[:len(base) // block_size * block_size].reshape(-1, block_size)
    power = np.mean(blocks ** 2, axis=1)
    if power.max() < 1e-6:
        raise ValueError("The recording has no usable speech.")
    cutoff = (np.flatnonzero(power > power.max() * 0.20)[-1] + 1) * block_size
    return base[:cutoff].astype(np.float32)


def simulated_room(speech):
    dry = np.pad(speech, (0, RATE // 2))
    wet = dry.copy()
    rng = np.random.default_rng(51)
    for offset in range(375, 6000, 83):
        amplitude = 0.22 * np.exp(-offset / 1900) * rng.choice([-1, 1])
        wet[offset:] += dry[:-offset] * amplitude
    scale = 0.8 / max(np.abs(wet).max(), np.abs(dry).max())
    return dry * scale, wet * scale


def db_ratio(signal, noise):
    return 10 * np.log10((np.mean(signal ** 2) + 1e-12) / (np.mean(noise ** 2) + 1e-12))


def write_wav(path, samples):
    with wave.open(str(path), "wb") as file:
        file.setparams((1, 2, RATE, 0, "NONE", "not compressed"))
        file.writeframes(as_pcm(samples).tobytes())


def as_pcm(samples):
    return np.clip(samples * 32768, -32768, 32767).astype("<i2")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("speech", type=Path)
    parser.add_argument("--stage", type=Path, default=Path(__file__).resolve().parent.parent / ".phase1")
    parser.add_argument("--npu-output", type=Path)
    args = parser.parse_args()
    torch.set_num_threads(2)
    model = load_localvqe(args.stage)

    speech = read_speech(args.speech)
    dry, wet = simulated_room(speech)
    if args.speech.resolve() in {(args.stage / f"room-echo-{kind}.wav").resolve()
                                for kind in ("dry", "wet", "cpu")}:
        parser.error("Use a source WAV outside the generated room-echo test paths.")
    with torch.inference_mode():
        microphone = torch.from_numpy(wet).unsqueeze(0)
        encoded = model(microphone, torch.zeros_like(microphone))
        result = model.decoder(encoded, length=len(wet)).squeeze(0).numpy()
    if result.shape != wet.shape or not np.isfinite(result).all():
        raise ValueError("The room-echo reference model produced invalid audio.")

    for kind, samples in (("dry", dry), ("wet", wet), ("cpu", result)):
        write_wav(args.stage / f"room-echo-{kind}.wav", samples)
    as_pcm(wet).tofile(args.stage / "room-echo-wet.pcm")
    boundary = len(speech)
    before = db_ratio(wet[:boundary], wet[boundary:])
    after = db_ratio(result[:boundary], result[boundary:])
    print(f"Speech-to-tail ratio {before:.2f} dB input, {after:.2f} dB output.")
    print(f"Speech-region signal to dry error {db_ratio(dry[:boundary], result[:boundary] - dry[:boundary]):.2f} dB.")
    if args.npu_output:
        accelerated = np.fromfile(args.npu_output, dtype="<i2").astype(np.float32) / 32768
        if accelerated.shape != wet.shape:
            raise ValueError(f"Expected {len(wet)} output samples, received {len(accelerated)}.")
        ratio = db_ratio(accelerated[:boundary], accelerated[boundary:])
        error = db_ratio(dry[:boundary], accelerated[:boundary] - dry[:boundary])
        print(f"NPU speech-to-tail ratio {ratio:.2f} dB. Speech-region signal to dry error {error:.2f} dB.")
        write_wav(args.stage / "room-echo-npu.wav", accelerated)


if __name__ == "__main__":
    main()
