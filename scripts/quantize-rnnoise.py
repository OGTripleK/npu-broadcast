import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

import numpy as np
import onnx
from onnx import helper, numpy_helper
from onnxruntime.quantization import CalibrationDataReader, QuantFormat, QuantType, quantize_static


class RecordedFrames(CalibrationDataReader):
    def __init__(self, model, recording):
        self.inputs = [
            (tensor.name, tuple(dim.dim_value for dim in tensor.type.tensor_type.shape.dim))
            for tensor in model.graph.input
        ]
        frame_width = sum(int(np.prod(shape)) for _, shape in self.inputs)
        if frame_width == 0 or any(0 in shape for _, shape in self.inputs):
            raise ValueError("Calibration requires the fixed input shapes from the RNNoise exporter.")
        if recording.stat().st_size % (frame_width * 4):
            raise ValueError("The recording ends with an incomplete RNNoise frame.")
        samples = np.fromfile(recording, dtype="<f4")
        if samples.size == 0 or not np.isfinite(samples).all():
            raise ValueError("The calibration recording must contain complete, finite RNNoise frames.")
        self.frames = samples.reshape(-1, frame_width)
        self.rewind()

    def rewind(self):
        self.iterator = iter(self.frames)

    def get_next(self):
        frame = next(self.iterator, None)
        if frame is None:
            return None
        sample = {}
        offset = 0
        for name, shape in self.inputs:
            width = int(np.prod(shape))
            sample[name] = frame[offset : offset + width].reshape(shape)
            offset += width
        return sample


def represent_matrices_as_convolutions(model):
    matrices = {weight.name: weight for weight in model.graph.initializer}
    replaced_weights = set()
    initializers = []
    nodes = []
    for node in model.graph.node:
        if node.op_type != "MatMul":
            nodes.append(node)
            continue

        value, weight_name = node.input
        weights = numpy_helper.to_array(matrices[weight_name])
        inputs, outputs = weights.shape
        prefix = f"{weight_name}_as_conv"
        initializers.extend([
            numpy_helper.from_array(weights.T.reshape(outputs, inputs, 1, 1), f"{prefix}_weights"),
            numpy_helper.from_array(np.array([1, inputs, 1, 1], dtype=np.int64), f"{prefix}_input_shape"),
            numpy_helper.from_array(np.array([1, outputs], dtype=np.int64), f"{prefix}_output_shape"),
        ])
        nodes.extend([
            helper.make_node("Reshape", [value, f"{prefix}_input_shape"], [f"{prefix}_input"]),
            helper.make_node(
                "Conv", [f"{prefix}_input", f"{prefix}_weights"], [f"{prefix}_output"],
                name=prefix, kernel_shape=[1, 1], strides=[1, 1], dilations=[1, 1],
                pads=[0, 0, 0, 0], group=1,
            ),
            helper.make_node("Reshape", [f"{prefix}_output", f"{prefix}_output_shape"], list(node.output)),
        ])
        replaced_weights.add(weight_name)

    model.graph.ClearField("node")
    model.graph.node.extend(nodes)
    initializers.extend(weight for name, weight in matrices.items() if name not in replaced_weights)
    model.graph.ClearField("initializer")
    model.graph.initializer.extend(initializers)
    return onnx.shape_inference.infer_shapes(model)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("model", type=Path)
    parser.add_argument("recording", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--convolutions", action="store_true")
    args = parser.parse_args()
    if args.output.resolve() in {args.model.resolve(), args.recording.resolve()}:
        parser.error("Output must differ from the model and calibration recording.")

    model = onnx.load(args.model)
    frames = RecordedFrames(model, args.recording)
    with TemporaryDirectory(dir=args.output.parent) as directory:
        input_path = args.model
        operators = ["MatMul"]
        if args.convolutions:
            input_path = Path(directory) / "rnnoise_conv.onnx"
            transformed = represent_matrices_as_convolutions(model)
            onnx.checker.check_model(transformed)
            onnx.save(transformed, input_path)
            operators = ["Conv"]
        quantize_static(
            input_path,
            args.output,
            frames,
            quant_format=QuantFormat.QDQ,
            op_types_to_quantize=operators,
            activation_type=QuantType.QInt8,
            weight_type=QuantType.QInt8,
            per_channel=False,
            extra_options={"ActivationSymmetric": True, "WeightSymmetric": True},
        )
    onnx.checker.check_model(str(args.output))
    print(f"Quantized RNNoise matrix multiplications using {len(frames.frames)} recorded frames.")


if __name__ == "__main__":
    main()
