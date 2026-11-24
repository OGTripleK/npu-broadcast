import argparse
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np
import onnx
from onnx import helper, numpy_helper
from onnx.utils import Extractor


DOMAIN = "npu.broadcast"


def round_channels(size):
    return (size + 7) // 8 * 8


def separate_matrices(model, destination, room_echo=False):
    graph = model.graph
    extractor = Extractor(model)
    producers = {name: node for node in graph.node for name in node.output}
    consumers = Counter(name for node in graph.node for name in node.input)
    consumers.update(tensor.name for tensor in graph.output)
    by_input = defaultdict(list)
    for node in graph.node:
        for name in node.input:
            by_input[name].append(node)
    convs = [node for node in graph.node if node.op_type == "Conv" and node.input[1] in producers
             and producers[node.input[1]].op_type == "DequantizeLinear"]
    if room_echo:
        convs = [node for node in convs if producers[node.input[1]].input[0].startswith(("model.mic_", "model.dec"))
                 and next((list(attr.ints) for attr in node.attribute if attr.name == "kernel_shape"), []) == [4, 4]]
    expected = 20 if room_echo else 10
    if len(convs) != expected:
        raise ValueError(f"Expected {expected} quantized projections, found {len(convs)}.")

    matrices = []
    replacements = {}
    removed = set()
    for index, conv in enumerate(convs):
        name = conv.name
        input_name = producers[producers[conv.input[0]].input[0]].input[0]
        [quantize] = by_input[conv.output[0]]
        [dequantize] = by_input[quantize.output[0]]
        output_name = dequantize.output[0]
        matrix = extractor.extract_model([input_name], [output_name])
        nodes = matrix.graph.node
        if Counter(node.op_type for node in nodes) != {
            "QuantizeLinear": 2, "DequantizeLinear": 4 if len(conv.input) == 3 else 3, "Conv": 1,
        }:
            raise ValueError(f"Unexpected quantization pattern for {name}.")
        for node in nodes:
            if any(value != output_name and consumers[value] != 1 for value in node.output):
                raise ValueError(f"Unexpected shared tensor in {name}.")
            removed.add(node.name)

        weight_dq = producers[conv.input[1]]
        weight_name = weight_dq.input[0]
        weight = next(weight for weight in matrix.graph.initializer if weight.name == weight_name)
        weights = numpy_helper.to_array(weight)
        outputs, inputs, _, _ = weights.shape
        input_shape = [value.dim_value for value in matrix.graph.input[0].type.tensor_type.shape.dim]
        output_shape = [value.dim_value for value in matrix.graph.output[0].type.tensor_type.shape.dim]
        if (len(input_shape) != 4 or len(output_shape) != 4 or min(input_shape + output_shape) <= 0 or
                input_shape[:2] != [1, inputs] or output_shape[:2] != [1, outputs]):
            raise ValueError(f"Expected fixed NCHW shape for {name}.")
        padded_in = round_channels(inputs)
        padded_out = round_channels(outputs)
        zero = next(item for item in matrix.graph.initializer if item.name == weight_dq.input[2])
        padded = np.pad(weights, ((0, padded_out - outputs), (0, padded_in - inputs), (0, 0), (0, 0)),
                        constant_values=int(numpy_helper.to_array(zero).item()))
        weight.CopyFrom(numpy_helper.from_array(padded, weight_name))
        if len(conv.input) == 3 and padded_out != outputs:
            bias_dq = producers[conv.input[2]]
            bias = next(item for item in matrix.graph.initializer if item.name == bias_dq.input[0])
            zero = next(item for item in matrix.graph.initializer if item.name == bias_dq.input[2])
            padded = np.pad(numpy_helper.to_array(bias), (0, padded_out - outputs),
                            constant_values=int(numpy_helper.to_array(zero).item()))
            bias.CopyFrom(numpy_helper.from_array(padded, bias.name))

        # Keep only public tensor shapes. Old inferred intermediate shapes predate padding.
        matrix.graph.ClearField("value_info")
        matrix.graph.input[0].type.tensor_type.shape.dim[1].dim_value = padded_in
        matrix.graph.output[0].type.tensor_type.shape.dim[1].dim_value = padded_out
        for node in nodes:
            for position, value in enumerate(node.input):
                if value == input_name:
                    node.input[position] = "input"
            for position, value in enumerate(node.output):
                if value == output_name:
                    node.output[position] = "output"
        matrix.graph.input[0].name = "input"
        matrix.graph.output[0].name = "output"
        onnx.checker.check_model(matrix)
        matrices.append((destination / f"matrix-{index}.onnx", matrix))
        attrs = {"matrix": index, "input_channels": inputs, "output_channels": outputs}
        if room_echo:
            attrs.update(input_height=input_shape[2], input_width=input_shape[3],
                         output_height=output_shape[2], output_width=output_shape[3])
        replacements[producers[output_name].name] = helper.make_node(
            "ConvolveOnNpu" if room_echo else "ProjectOnNpu",
            [input_name], [output_name], domain=DOMAIN, name=name, **attrs)

    # Insert each replacement after the original data producer in topological order.
    host_nodes = [replacements.get(node.name, node) for node in graph.node
                  if node.name in replacements or node.name not in removed]
    used = {value for node in host_nodes for value in node.input}
    kept_initializers = [item for item in graph.initializer if item.name in used]
    graph.ClearField("node")
    graph.node.extend(host_nodes)
    graph.ClearField("initializer")
    graph.initializer.extend(kept_initializers)
    graph.ClearField("value_info")
    model.opset_import.append(helper.make_opsetid(DOMAIN, 1))
    onnx.checker.check_model(model)
    return matrices


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("model", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--room-echo", action="store_true")
    args = parser.parse_args()
    name = "room_echo_host.onnx" if args.room_echo else "rnnoise_host.onnx"
    if (args.destination / name).resolve() == args.model.resolve():
        parser.error("The destination must not overwrite the source model.")
    model = onnx.shape_inference.infer_shapes(onnx.load(args.model), strict_mode=True)
    matrices = separate_matrices(model, args.destination, args.room_echo)
    args.destination.mkdir(parents=True, exist_ok=True)
    for path, matrix in matrices:
        onnx.save(matrix, path)
    onnx.save(model, args.destination / name)
    print(f"Wrote {len(matrices)} isolated NPU convolution models and the host graph to {args.destination}")


if __name__ == "__main__":
    main()
