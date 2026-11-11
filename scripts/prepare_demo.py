#!/usr/bin/env python3
"""Prepare the 1440p demo from locked sources and user-owned videos, never copied caches."""
import argparse
from fractions import Fraction
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
from bootstrap import download, run

ROOT=Path(__file__).resolve().parents[1]
PROFILE=ROOT / "examples/nvr/demo/1440p"


def validate_videos(path):
    videos=[(path.resolve().parent / line.strip()).resolve() for line in path.read_text().splitlines() if line.strip() and not line.lstrip().startswith("#")]
    if len(videos)!=16 or len(set(videos))!=16:
        raise ValueError("Exactly 16 distinct user-provided videos are required")
    for v in videos:
        if not v.is_file() or any(c in str(v) for c in "\r\n"):
            raise ValueError(f"Invalid video path: {v}")
        x=json.loads(subprocess.check_output(["ffprobe","-v","error","-select_streams","v:0","-show_entries","stream=codec_name,width,height,r_frame_rate,avg_frame_rate","-of","json",str(v)]))
        streams=x.get("streams",[])
        if len(streams)!=1:
            raise ValueError(f"Missing video stream: {v}")
        s=streams[0]
        if (s["codec_name"],s["width"],s["height"])!=("hevc",2560,1440) or Fraction(s["avg_frame_rate"])!=30 or Fraction(s["r_frame_rate"])!=30:
            raise ValueError(f"Expected 2560x1440 30fps HEVC, got {s}: {v}; no automatic transcoding")
    return videos


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--videos-list",type=Path,required=True)
    p.add_argument("--output",type=Path,default=ROOT / "artifacts/1440p-demo")
    p.add_argument("--backend",choices=["gpu","npu","gpu+npu"],default="npu")
    p.add_argument("--ryzen-ai-venv",type=Path)
    p.add_argument("--check-videos",action="store_true")
    a=p.parse_args()
    videos=validate_videos(a.videos_list)
    if a.check_videos:
        print("Validated 16 user-provided 1440p30 HEVC videos");return
    if not a.ryzen_ai_venv:
        p.error("--ryzen-ai-venv is required for the MobileCLIP ORT SDK")
    raw=subprocess.check_output(["bash","-c","source scripts/env.sh && env -0"],cwd=ROOT)
    environment={item.split(b"=",1)[0].decode():item.split(b"=",1)[1].decode() for item in raw.split(b"\0") if b"=" in item}
    os.environ.clear();os.environ.update(environment)
    host_args = ["--ort-only"] if a.backend == "gpu" else []
    run("/usr/bin/python3", ROOT / "scripts/host.py", "--runtime", "--ryzen-ai-venv", a.ryzen_ai_venv.resolve(), *host_args)
    out=a.output.resolve()
    if (out / "config.json").exists():
        raise ValueError("Runtime already exists; choose a new --output to preserve its settings")
    assets=out / "assets";assets.mkdir(parents=True,exist_ok=True)
    d=ROOT / ".deps/model-downloads";d.mkdir(parents=True,exist_ok=True)
    lock=json.loads((ROOT / "dependencies.lock.json").read_text())
    names={"yolo":"yolo.pt","reid_source":"osnet.py","reid_weights":"reid.pth","mobileclip_prepare":"mobileclip2_prepare.py","mobileclip_weights":"mobileclip.safetensors"}
    for key,name in names.items():
        entry=lock["demo_models"][key];download(entry["url"],d/name,entry["sha256"])
    run(sys.executable,ROOT / "scripts/setup_model_export.py")
    export_record=assets / "export-sources.json"
    if not export_record.exists() or json.loads(export_record.read_text())!=lock["demo_models"]:
        run(ROOT / ".deps/model-export/bin/python",ROOT / "scripts/export_demo_models.py",assets,d)
        export_record.write_text(json.dumps(lock["demo_models"],indent=2)+"\n")
    compiler=ROOT / "build/src/gst/amdva-compile-model"
    for name,source,b,h,w in [("detector-b1","detector",1,384,640),("detector-b4","detector",4,384,640),("reid-b4","reid",4,256,128)]:
        run(compiler,assets/(source+".onnx"),assets/(name+".mxr"),f"--batch={b}",f"--input-shape=images:{b},3,{h},{w}","--precision=fp16")
    bundle=assets / "mobileclip"
    if a.backend in ("gpu","gpu+npu"):
        run(sys.executable,ROOT / "scripts/prepare_mobileclip_device.py",bundle)
    if a.backend in ("npu","gpu+npu"):
        run(ROOT / "examples/nvr/scripts/prepare-mobileclip-npu.sh",a.ryzen_ai_venv.resolve(),bundle,d / "mobileclip2_prepare.py")
    spec=importlib.util.spec_from_file_location("demo_manage",PROFILE / "manage.py")
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
    inputs=out / "input-videos";inputs.mkdir(exist_ok=True)
    names=[]
    for i,v in enumerate(videos):
        name=f"M{i+1:02d}.mp4";link=inputs/name
        if link.is_symlink():link.unlink()
        link.symlink_to(v);names.append(name)
    m.write(assets/"assets.json",{"schema":1,"videos":names,"files":{},"backends":[]})
    m.refresh(assets)
    m.configure(argparse.Namespace(assets=assets,videos=inputs,output=out,backend=a.backend,venv=a.ryzen_ai_venv,rtsp_port=18570,dashboard_port=18572,cpu_name="",force=False))
    launcher=out / "run-demo.sh"
    launcher.write_text('#!/usr/bin/env bash\nset -euo pipefail\nruntime="$(cd -- "$(dirname -- "$0")" && pwd)"\nexec '+__import__('shlex').quote(str(PROFILE / "run.sh"))+' "$@" "$runtime"\n')
    launcher.chmod(0o755)
    m.write(out / "preparation.json",{"sources":lock["demo_models"],"host":json.loads((ROOT / ".deps/host.json").read_text()),"videos":[str(v) for v in videos],"generation":{"detector":{"input":"images","width":640,"height":384,"batches":[1,4],"precision":"fp16"},"reid":{"input":"images","width":128,"height":256,"batch":4,"precision":"fp16"},"mobileclip":{"input":"images","width":256,"height":256,"backend":a.backend}}})
    print(f"Prepared {launcher}")


if __name__=="__main__":
    try:main()
    except (ValueError,OSError,subprocess.SubprocessError) as e:sys.exit(str(e))
