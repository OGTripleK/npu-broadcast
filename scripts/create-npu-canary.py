from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper


def create_graph(quantized, recurrent_shape=False):
    if recurrent_shape:
        input_shape, output_shape = [1, 384, 1, 1], [1, 1152, 1, 1]
        weights = np.zeros((1152, 384, 1, 1), dtype=np.int8)
        weights[:, :72, 0, 0] = 1
    else:
        input_shape, output_shape = [1, 8, 16, 16], [1, 8, 14, 14]
        weights = np.ones((8, 8, 3, 3), dtype=np.int8 if quantized else np.float32)
    input_tensor = helper.make_tensor_value_info("input", TensorProto.FLOAT, input_shape)
    output_tensor = helper.make_tensor_value_info("output", TensorProto.FLOAT, output_shape)
    make_node = helper.make_node
    conv_attributes = {
        "kernel_shape": list(weights.shape[2:]),
        "strides": [1, 1],
        "dilations": [1, 1],
        "pads": [0, 0, 0, 0],
        "group": 1,
    }
    if quantized:
        initializers = [
            numpy_helper.from_array(np.array(1.0, dtype=np.float32), "scale"),
            numpy_helper.from_array(np.array(0, dtype=np.int8), "zero_point"),
            numpy_helper.from_array(weights, "weights_quantized"),
        ]
        nodes = [
            make_node("QuantizeLinear", ["input", "scale", "zero_point"], ["input_quantized"]),
            make_node("DequantizeLinear", ["input_quantized", "scale", "zero_point"], ["input_float"]),
            make_node("DequantizeLinear", ["weights_quantized", "scale", "zero_point"], ["weights"]),
            make_node("Conv", ["input_float", "weights"], ["conv_out"], **conv_attributes),
            make_node("QuantizeLinear", ["conv_out", "scale", "zero_point"], ["output_quantized"]),
            make_node("DequantizeLinear", ["output_quantized", "scale", "zero_point"], ["output"]),
        ]
    else:
        initializers = [numpy_helper.from_array(weights, "weights")]
        nodes = [make_node("Conv", ["input", "weights"], ["output"], **conv_attributes)]
    return helper.make_graph(nodes, "npu_canary", [input_tensor], [output_tensor], initializers)


def main():
    model_dir = Path(__file__).resolve().parent.parent / ".phase1" / "models"
    for name, quantized, recurrent_shape in [
        ("npu_canary", False, False),
        ("npu_canary_int8", True, False),
        ("npu_canary_gru_shape", True, True),
    ]:
        graph = create_graph(quantized, recurrent_shape)
        model = helper.make_model(graph, ir_version=9, opset_imports=[helper.make_opsetid("", 13)])
        onnx.checker.check_model(model)
        path = model_dir / f"{name}.onnx"
        onnx.save(model, path)
        print(path)


if __name__ == "__main__":
    main()
