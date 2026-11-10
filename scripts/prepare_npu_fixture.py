#!/usr/bin/env python3
"""Prepare an offline static, two-output image model for amdnpu validation.

Run inside the prepared Ryzen AI environment. Compilation happens here, before
the application starts; the runtime only consumes the resulting cache.
"""
import argparse
import json
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper
import onnxruntime as ort

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("destination", type=Path)
parser.add_argument("provider_config", type=Path)
args = parser.parse_args()
root = args.destination.resolve()
root.mkdir(parents=True, exist_ok=True)
nodes = [helper.make_node("Conv", ["images", "weight", "bias"], ["conv"], kernel_shape=[1, 1]),
         helper.make_node("Relu", ["conv"], ["features"]),
         helper.make_node("GlobalAveragePool", ["features"], ["pooled"])]
graph = helper.make_graph(nodes, "amdva-npu-fixture",
    [helper.make_tensor_value_info("images", TensorProto.FLOAT, [1, 3, 32, 32])],
    [helper.make_tensor_value_info("features", TensorProto.FLOAT, [1, 8, 32, 32]),
     helper.make_tensor_value_info("pooled", TensorProto.FLOAT, [1, 8, 1, 1])],
    [numpy_helper.from_array(np.full((8, 3, 1, 1), .25, dtype=np.float32), "weight"),
     numpy_helper.from_array(np.full(8, .5, dtype=np.float32), "bias")])
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)], ir_version=9)
onnx.checker.check_model(model)
onnx.save(model, root / "image.onnx")
(root / "provider.json").write_bytes(args.provider_config.read_bytes())
options = ort.SessionOptions()
options.add_session_config_entry("session.disable_cpu_ep_fallback", "1")
session = ort.InferenceSession(str(root / "image.onnx"), sess_options=options,
    providers=["VitisAIExecutionProvider"], provider_options=[{
        "config_file": str(root / "provider.json"), "cache_dir": str(root / "cache"),
        "cache_key": "fixture", "enable_cache_file_io_in_mem": "0"}])
for fill in (0., 1.):
    outputs = session.run(None, {"images": np.full((1, 3, 32, 32), fill, dtype=np.float32)})
    for output in outputs:
        np.testing.assert_allclose(output, .5 + .75 * fill, rtol=.02, atol=.02)
(root / "fixture.json").write_text(json.dumps({"input": [1, 3, 32, 32],
    "outputs": ["features", "pooled"], "cache_key": "fixture"}, indent=2) + "\n")
print(root)
