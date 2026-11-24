import argparse
import math
import wave
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort
from onnxruntime.quantization import CalibrationDataReader, QuantFormat, QuantType, quantize_static
from onnxruntime.tools.symbolic_shape_infer import SymbolicShapeInference
import torch
from torch import nn
from torch.nn import functional as F

from localvqe_reference import load_localvqe


class HopCalibration(CalibrationDataReader):
    def __init__(self, samples):
        self.samples = samples
        self.rewind()

    def rewind(self):
        self.iterator = iter(self.samples)

    def get_next(self):
        return next(self.iterator, None)


class RoomEchoStep(nn.Module):
    """One 16 ms LocalVQE hop with explicit delay lines and S4D state."""

    def __init__(self, model):
        super().__init__()
        self.model = model

    def forward(self, window, *state):
        model = self.model
        next_state = []

        def history(frame, count):
            previous = state[len(next_state)] if state else torch.zeros_like(frame).expand(-1, -1, count, -1)
            combined = torch.cat((previous, frame), dim=2)
            next_state.append(combined[:, :, 1:, :])
            return combined

        def conv(layer, frame):
            height, width = layer.kernel_size
            if height != 1:
                frame = history(frame, height - 1)
            left = (width - 1) // 2
            return layer(F.pad(frame, (left, width - 1 - left)))

        def residual(block, value):
            return value + block.act(conv(block.conv, block.norm(value)))

        def encoder(block, value):
            return residual(block.resblock, block.act(conv(block.conv, block.norm(value))))

        def decoder(block, value, skip):
            value = value + block.skip_conv(block.skip_norm(skip))
            value = residual(block.resblock, value)
            value = conv(block.deconv.conv, block.deconv.norm(value))
            value = value.reshape(1, 2, value.shape[1] // 2, 1, value.shape[-1])
            value = value.permute(0, 2, 3, 1, 4).reshape(1, value.shape[2], 1, value.shape[-1] * 2)
            return value if block.is_last else block.act(value)

        spectrum = model.encoder.conv(window.unsqueeze(1)).reshape(1, model.n_freqs, 2, 1).transpose(2, 3)
        mic_e1 = encoder(model.mic_enc1, model.fe_mic(spectrum))
        mic_e2 = encoder(model.mic_enc2, mic_e1)
        ref_e1 = encoder(model.far_enc1, model.fe_ref(torch.zeros_like(spectrum)))
        ref_e2 = encoder(model.far_enc2, ref_e1)

        align = model.align
        query = align.pconv_mic(mic_e2)
        keys = history(align.pconv_ref(ref_e2), align.dmax - 1)
        scores = (query * keys).sum(dim=-1).unsqueeze(2) / math.sqrt(query.shape[-1])
        attention = torch.softmax(conv(align.conv[1], scores) / align.temperature, dim=-1)
        reference = history(ref_e2, align.dmax - 1)
        aligned = (reference * attention.transpose(2, 3)).sum(dim=2, keepdim=True)
        mic_e3 = encoder(model.mic_enc3, torch.cat((mic_e2, aligned), dim=1))
        mic_e4 = encoder(model.mic_enc4, mic_e3)
        mic_e5 = encoder(model.mic_enc5, mic_e4)

        bottleneck = model.bottleneck
        flat = mic_e5.reshape(1, -1)
        projected = bottleneck.input_proj(flat)
        offset = len(next_state)
        real, imag = state[offset:offset + 2] if state else (torch.zeros_like(projected), torch.zeros_like(projected))
        magnitude = torch.exp(-F.softplus(bottleneck.A_log_rate).clamp(min=0.01))
        a_real = magnitude * torch.cos(bottleneck.A_theta)
        a_imag = magnitude * torch.sin(bottleneck.A_theta)
        real_next = a_real * real - a_imag * imag + bottleneck.B_real * projected
        imag_next = a_real * imag + a_imag * real + bottleneck.B_imag * projected
        next_state.extend((real_next, imag_next))
        projected = bottleneck.C_real * real_next - bottleneck.C_imag * imag_next
        bottleneck_output = (bottleneck.output_proj(projected) + bottleneck.D * flat).reshape_as(mic_e5)

        d5 = decoder(model.dec5, bottleneck_output, mic_e5)[..., :mic_e4.shape[-1]]
        d4 = decoder(model.dec4, d5, mic_e4)[..., :mic_e3.shape[-1]]
        d3 = decoder(model.dec3, d4, mic_e3)[..., :mic_e2.shape[-1]]
        d2 = decoder(model.dec2, d3, mic_e2)[..., :mic_e1.shape[-1]]
        mask = decoder(model.dec1, d2, mic_e1)[..., :model.n_freqs]
        spectra = history(spectrum.permute(0, 3, 2, 1), 2).permute(0, 3, 2, 1)
        enhanced = model.mask(mask.expand(-1, -1, 3, -1), spectra)[:, :, -1, :]
        return (model.decoder.linear(enhanced.reshape(1, -1)), *next_state)


def read_room_fixture(path):
    with wave.open(str(path), "rb") as file:
        if (file.getnchannels(), file.getsampwidth(), file.getframerate()) != (1, 2, 16000):
            raise ValueError("Use a 16 kHz mono 16-bit PCM WAV.")
        pcm = np.frombuffer(file.readframes(file.getnframes()), dtype="<i2").astype(np.float32) / 32768
    if pcm.size < 512:
        raise ValueError("The fixture needs at least 512 samples.")
    return torch.from_numpy(pcm).unsqueeze(0)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage", type=Path, default=Path(__file__).resolve().parent.parent / ".phase1")
    parser.add_argument("--quantize", action="store_true")
    args = parser.parse_args()
    torch.set_num_threads(2)
    model = load_localvqe(args.stage)
    step = RoomEchoStep(model).eval()
    wave_input = read_room_fixture(args.stage / "room-echo-wet.wav")
    windows = F.pad(wave_input, (256, 256)).unfold(1, 512, 256)[0]
    state = ()
    frames = []
    with torch.no_grad():
        for window in windows:
            result, *state = step(window.unsqueeze(0), *state)
            frames.append(result)
        encoded = model(wave_input, torch.zeros_like(wave_input))
        reference = model.decoder.linear(encoded.permute(0, 2, 1, 3).reshape(1, len(windows), 512))[0]
        torch.testing.assert_close(torch.cat(frames), reference, rtol=2e-3, atol=3e-5)
        print(f"Streaming Torch matched the batch model across {len(windows)} hops.")

        model_path = args.stage / "models" / "room_echo_step.onnx"
        state_names = [f"state_{index}" for index in range(len(state))]
        zeros = [torch.zeros_like(value) for value in state]
        torch.onnx.export(
            step, (windows[0].unsqueeze(0), *zeros), model_path,
            input_names=["window", *state_names],
            output_names=["enhanced_window", *(f"next_{name}" for name in state_names)],
            opset_version=17, dynamo=False,
        )
    graph = SymbolicShapeInference.infer_shapes(onnx.load(model_path), auto_merge=True, guess_output_rank=True)
    onnx.checker.check_model(graph)
    onnx.save(graph, model_path)
    options = ort.SessionOptions()
    options.intra_op_num_threads = 2
    session = ort.InferenceSession(str(model_path), options, providers=["CPUExecutionProvider"])
    state = [value.numpy() for value in zeros]
    exported = []
    calibration = []
    for index, window in enumerate(windows):
        values = {"window": window.unsqueeze(0).numpy(), **dict(zip(state_names, state, strict=True))}
        if args.quantize and index % 4 == 0:
            calibration.append(values)
        enhanced, *state = session.run(None, values)
        exported.append(enhanced)
    np.testing.assert_allclose(np.concatenate(exported), reference.numpy(), rtol=2e-3, atol=3e-5)
    print(f"Streaming ONNX matched the batch model. {len(state)} explicit state tensors. {model_path}")
    if args.quantize:
        weight_ranks = {item.name: len(item.dims) for item in graph.graph.initializer}
        convs = [node.name for node in graph.graph.node
                 if node.op_type == "Conv" and weight_ranks.get(node.input[1]) == 4]
        output = model_path.with_name("room_echo_int8.onnx")
        quantize_static(
            model_path, output, HopCalibration(calibration), quant_format=QuantFormat.QDQ,
            nodes_to_quantize=convs, activation_type=QuantType.QInt8, weight_type=QuantType.QInt8,
            per_channel=False, extra_options={"ActivationSymmetric": True, "WeightSymmetric": True,
                                              "DedicatedQDQPair": True},
        )
        onnx.checker.check_model(str(output))
        print(f"Quantized {len(convs)} spatial convolutions using {len(calibration)} streaming hops. {output}")


if __name__ == "__main__":
    main()
