import hashlib
import sys

import torch


CHECKPOINT_SHA256 = "ff6885e7c8d7d29a8ce963303dcd668ae0f2a7bdafae28631292fe6f06f7cd77"


def load_localvqe(stage):
    checkpoint = stage / "downloads" / "localvqe-v1.2-1.3M.pt"
    with checkpoint.open("rb") as file:
        if hashlib.file_digest(file, "sha256").hexdigest() != CHECKPOINT_SHA256:
            raise ValueError("LocalVQE v1.2 checkpoint hash mismatch.")
    sys.path.insert(0, str(stage / "localvqe" / "pytorch"))
    from localvqe.model import LocalVQE

    model = LocalVQE().eval()
    weights = torch.load(checkpoint, map_location="cpu", weights_only=True)
    model.load_state_dict(weights["model_state_dict"], strict=True)
    model.align.fold_temperature()
    return model
