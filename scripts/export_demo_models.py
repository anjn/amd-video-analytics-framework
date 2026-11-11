#!/usr/bin/env python3
"""CPU export of locked demo checkpoints, isolated from the inference process."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil


def main():
    p=argparse.ArgumentParser()
    p.add_argument("assets", type=Path)
    p.add_argument("downloads", type=Path)
    a=p.parse_args()
    import torch
    from ultralytics import YOLO
    torch.set_num_threads(4)
    os.environ["YOLO_AUTOINSTALL"]="false"
    local=a.assets / "yolo11n.pt"
    shutil.copyfile(a.downloads / "yolo.pt",local)
    detector=YOLO(str(local))
    out=detector.export(format="onnx",imgsz=(384,640),dynamic=True,batch=4,opset=13,simplify=True,device="cpu")
    shutil.copyfile(out,a.assets / "detector.onnx")
    local.unlink()
    spec=importlib.util.spec_from_file_location("osnet", a.downloads / "osnet.py")
    mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
    reid=mod.osnet_x0_25(num_classes=1,pretrained=False).eval()
    ckpt=torch.load(a.downloads / "reid.pth",map_location="cpu",weights_only=True)
    state=ckpt.get("state_dict",ckpt)
    state={k.removeprefix("module."):v for k,v in state.items() if not k.removeprefix("module.").startswith("classifier.")}
    missing,extra=reid.load_state_dict(state,strict=False)
    if set(missing)!={"classifier.weight","classifier.bias"} or extra:
        raise ValueError(f"Unexpected OSNet checkpoint: {missing} {extra}")
    torch.onnx.export(reid,torch.zeros(1,3,256,128),str(a.assets / "reid.onnx"),input_names=["images"],output_names=["features"],dynamic_axes={"images":{0:"batch"},"features":{0:"batch"}},opset_version=13,dynamo=False)
    spec=importlib.util.spec_from_file_location("mc_prepare",a.downloads / "mobileclip2_prepare.py")
    mc=importlib.util.module_from_spec(spec);spec.loader.exec_module(mc)
    import huggingface_hub
    # Only the already checksum-verified checkpoint can be handed to open_clip.
    def checkpoint(repo,filename,revision):
        if (repo,filename,revision)!=(mc.REPO,"open_clip_model.safetensors",mc.REVISION):
            raise ValueError("Unexpected MobileCLIP checkpoint request")
        return str(a.downloads / "mobileclip.safetensors")
    huggingface_hub.hf_hub_download=checkpoint
    bundle=a.assets / "mobileclip";bundle.mkdir(exist_ok=True)
    mc.export(bundle)


if __name__=="__main__":main()
