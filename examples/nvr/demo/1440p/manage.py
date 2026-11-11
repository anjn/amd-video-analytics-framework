#!/usr/bin/env python3
"""Export the prepared 1440p demo assets and generate machine-local NVR settings."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys

PROFILE = Path(__file__).resolve().parent


def read(path):
    return json.loads(path.read_text())


def write(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")


def digest(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def inside(root, name):
    if not isinstance(name, str):
        raise ValueError("Asset name must be a string")
    path = root / name
    if Path(name).is_absolute() or not path.resolve().is_relative_to(root.resolve()):
        raise ValueError(f"Asset path escapes bundle: {name}")
    return path


def bundle_files(bundle):
    files = {}
    def add(name):
        source = inside(bundle, name)
        if not source.is_file():
            raise ValueError(f"Required bundle file missing: {source}")
        files["mobileclip/" + name] = source
    manifest = read(bundle / "manifest.json")
    for name, suffix in [("image", ".onnx"), ("text", ".onnx"), ("tokenizer", ".json")]:
        if digest(bundle / (name + suffix)) != manifest[name + "_sha256"]:
            raise ValueError(f"MobileCLIP {name} checksum mismatch")
    for name in ["manifest.json", "image.onnx", "text.onnx", "tokenizer.json"]:
        add(name)
    available = []
    for mode, name in [("gpu", "gpu-device.json"), ("npu", "npu.json")]:
        if not (bundle / name).is_file():
            continue
        meta = read(bundle / name)
        add(name)
        for relative, expected_hash in meta["artifacts"].items():
            source = inside(bundle, relative)
            if digest(source) != expected_hash:
                raise ValueError(f"Bundle checksum mismatch: {source}")
            add(relative)
        if meta["image_sha256"] != manifest["image_sha256"]:
            raise ValueError(f"Compiled {mode} image checksum mismatch")
        if mode == "gpu":
            for batch in [1, 4]:
                add(f"gpu-device-b{batch}.mxr")
                add(f"gpu-device-b{batch}.mxr.manifest.json")
        if mode == "npu":
            if not meta["cache_key"] or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_" for c in meta["cache_key"]):
                raise ValueError("Invalid NPU cache key")
            add(f"npu-cache/{meta['cache_key']}/context.json")
            add("npu-config.json")
            cache = inside(bundle, "npu-cache/" + meta["cache_key"])
            # Preserve all prepared cache members, including SDK-specific files.
            for source in cache.rglob("*"):
                if source.is_file():
                    add(source.relative_to(bundle).as_posix())
        available.append(mode)
    return files, available


def model_files(root):
    files = {}
    for name, batch in [("detector-b4.mxr", 4), ("detector-b1.mxr", 1), ("reid-b4.mxr", 4)]:
        model = root / name
        sidecar = root / (name + ".manifest.json")
        meta = read(sidecar)
        if meta.get("schema") != 3 or meta.get("rocm") != "10.1.0" or meta.get("migraphx") != "2.18.0":
            raise ValueError(f"Recompile model for ROCm 10.1.0 / MIGraphX 2.18.0: {model}")
        if meta["sha256"] != digest(model) or meta["batch"] != batch or meta["offload_copy"]:
            raise ValueError(f"Invalid GPU model or sidecar: {model}")
        files[name] = model
        files[sidecar.name] = sidecar
    return files


def refresh(root):
    meta = read(root / "assets.json")
    files = model_files(root)
    bundle, available = bundle_files(root / "mobileclip")
    files.update(bundle)
    meta["backends"] = available
    meta["files"] = {name: {"sha256": digest(path), "bytes": path.stat().st_size} for name, path in files.items()}
    write(root / "assets.json", meta)
    print("Refreshed asset inventory after validating model/bundle manifests")


def export(args):
    output = args.output.resolve()
    if output.exists():
        raise ValueError(f"Export destination already exists: {output}")
    config_path = args.config.resolve()
    config = read(config_path)
    search = config["search"]
    if search.get("embedding_model") != "mobileclip2-s2":
        raise ValueError("This profile exports MobileCLIP2-S2, not EmbeddingGemma")
    files = {}

    def add(source, name):
        source = source.resolve()
        if not source.is_file():
            raise ValueError(f"Required file missing: {source}")
        files[name] = source

    for key, name in [("model", "detector-b4.mxr"), ("single_model", "detector-b1.mxr")]:
        source = config_path.parent / config[key]
        add(source, name)
        add(Path(str(source) + ".manifest.json"), name + ".manifest.json")
    reid = config_path.parent / search["reid_model"]
    add(reid, "reid-b4.mxr")
    add(Path(str(reid) + ".manifest.json"), "reid-b4.mxr.manifest.json")
    info_path = config_path.parent / config["model_info"]
    info = read(info_path)
    labels = info_path.parent / info.pop("labels")
    expected = read(PROFILE / "model-info.json")
    expected.pop("labels")
    if info != expected or labels.read_text().splitlines() != (PROFILE / "coco80.txt").read_text().splitlines():
        raise ValueError("Expected the reviewed 640x384 COCO detector metadata and labels")
    bundle = (config_path.parent / search["mobileclip_bundle"]).resolve()
    prepared, available = bundle_files(bundle)
    files.update(prepared)
    required = {"gpu", "npu"} if search["mobileclip_backend"] == "gpu+npu" else {search["mobileclip_backend"]}
    if not required.issubset(available):
        raise ValueError("Selected embedding backend has no prepared metadata")
    videos = []
    for line in args.videos_list.read_text().splitlines():
        if line.strip() and not line.lstrip().startswith("#"):
            path = (args.videos_list.resolve().parent / line.strip()).resolve()
            if not path.is_file():
                raise ValueError(f"Video missing: {path}")
            videos.append(path)
    if len(videos) != 16 or len({v.name for v in videos}) != 16:
        raise ValueError("Exactly 16 videos with unique filenames are required")
    for name, batch in [("detector-b4.mxr", 4), ("detector-b1.mxr", 1), ("reid-b4.mxr", 4)]:
        meta = read(files[name + ".manifest.json"])
        if meta["sha256"] != digest(files[name]) or meta["batch"] != batch or meta["offload_copy"]:
            raise ValueError(f"Invalid source model/manifest: {files[name]}")
    # Validate everything before writing the export; never alter the source assets.
    manifest = {"schema": 1, "backends": available, "files": {}, "videos": [v.name for v in videos]}
    for name, source in files.items():
        target = inside(output, name)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        manifest["files"][name] = {"sha256": digest(target), "bytes": target.stat().st_size}
    if args.include_videos:
        (output / "videos").mkdir()
        for video in videos:
            shutil.copyfile(video, output / "videos" / video.name)
    write(output / "assets.json", manifest)
    refresh(output)
    print(f"Exported {len(files)} asset files to {output}; videos {'included' if args.include_videos else 'must be transferred separately'}")


def verify_assets(root, backend):
    meta = read(root / "assets.json")
    if meta["schema"] != 1 or len(meta["videos"]) != 16:
        raise ValueError("Unsupported assets manifest")
    if len(set(meta["videos"])) != 16 or any(not isinstance(v, str) or v != Path(v).name or v in [".", ".."] for v in meta["videos"]):
        raise ValueError("Video entries must be 16 unique filenames")
    required = {"gpu", "npu"} if backend == "gpu+npu" else {backend}
    if not required.issubset(meta["backends"]):
        raise ValueError(f"Missing prepared backend: {backend}")
    mandatory = set(model_files(root))
    prepared, available = bundle_files(root / "mobileclip")
    mandatory.update(prepared)
    if not mandatory.issubset(meta["files"]):
        raise ValueError("Incomplete asset inventory")
    for name, entry in meta["files"].items():
        path = inside(root, name)
        if not path.is_file() or digest(path) != entry["sha256"]:
            raise ValueError(f"Asset missing or changed: {path}; export again after re-preparing models")
    return meta


def check(runtime):
    launch = read(runtime / "demo.json")
    config = read(runtime / "config.json")
    root = (runtime / launch["assets"]).resolve()
    if config["search"]["embedding_model"] != "mobileclip2-s2":
        raise ValueError("This launch profile supports MobileCLIP2-S2; configure EmbeddingGemma separately")
    verify_assets(root, config["search"]["mobileclip_backend"])
    for actual, expected in [(config["model"], root / "detector-b4.mxr"),
                             (config["single_model"], root / "detector-b1.mxr"),
                             (config["search"]["reid_model"], root / "reid-b4.mxr"),
                             (config["search"]["mobileclip_bundle"], root / "mobileclip")]:
        if (runtime / actual).resolve() != expected.resolve():
            raise ValueError("Config paths no longer refer to the verified asset bundle")
    info_path = runtime / config["model_info"]
    if not (info_path.parent / read(info_path)["labels"]).is_file():
        raise ValueError("Detector labels missing")
    for key in ["model", "single_model", "model_info"]:
        if not (runtime / config[key]).is_file():
            raise ValueError(f"Required {key} missing")
    video_paths = (runtime / "videos.txt").read_text().splitlines()
    if len(video_paths) != 16:
        raise ValueError("Expected exactly 16 fixture videos")
    for name in video_paths:
        if not (runtime / name).is_file():
            raise ValueError(f"Video missing: {name}")
    backend = config["search"]["mobileclip_backend"]
    venv = config["search"].get("ryzen_ai_venv") or os.environ.get("AMDVA_RYZEN_AI_VENV") or os.environ.get("VIRTUAL_ENV")
    if backend != "gpu" or venv:
        if not venv or not (runtime / venv / "bin/activate").is_file() or not (runtime / venv / "onnxruntime/lib").is_dir():
            raise ValueError("Set the target machine's prepared Ryzen AI venv")
    print("Asset checksums, videos and configuration paths OK (hardware/SDK execution is checked separately)")
    if backend != "gpu":
        print("NPU: cached context metadata may contain preparation-machine paths; reprepare in the final bundle location and run the hardware validator before relying on this deployment.")


def configure(args):
    root = args.assets.resolve()
    meta = verify_assets(root, args.backend)
    output = args.output.resolve()
    names = meta["videos"]
    videos = [(args.videos.resolve() / name) for name in names]
    if any(not v.is_file() or "\n" in str(v) or "\r" in str(v) for v in videos):
        raise ValueError("All 16 named videos must exist in --videos")
    if args.rtsp_port == args.dashboard_port or not all(1 <= p <= 65535 for p in [args.rtsp_port, args.dashboard_port]):
        raise ValueError("Use different RTSP/dashboard ports in 1..65535")
    if (output / "config.json").exists() and not args.force:
        raise ValueError("Runtime config already exists; use another output or --force (stop the demo first)")
    if args.venv:
        venv = args.venv.resolve()
        if not (venv / "bin/activate").is_file() or not (venv / "onnxruntime/lib").is_dir():
            raise ValueError("Expected a prepared Ryzen AI venv with onnxruntime/lib")
    elif args.backend != "gpu":
        raise ValueError("NPU requires --venv pointing to the target machine's Ryzen AI environment")
    output.mkdir(parents=True, exist_ok=True)
    rel = lambda path: os.path.relpath(path, output)
    config = read(PROFILE / "config.template.json")
    config.update(model=rel(root / "detector-b4.mxr"), single_model=rel(root / "detector-b1.mxr"))
    search = config["search"]
    search.update(reid_model=rel(root / "reid-b4.mxr"), mobileclip_bundle=rel(root / "mobileclip"),
                  mobileclip_backend=args.backend, ryzen_ai_venv=str(args.venv.resolve()) if args.venv else "")
    for source in config["sources"]:
        port = args.dashboard_port if source["id"] == "resource-monitor" else args.rtsp_port
        stream = "dashboard" if source["id"] == "resource-monitor" else source["id"]
        source["uri"] = f"rtsp://127.0.0.1:{port}/{stream}"
    shutil.copyfile(PROFILE / "model-info.json", output / "model-info.json")
    shutil.copyfile(PROFILE / "coco80.txt", output / "coco80.txt")
    if not (output / "config.json.ui.json").exists():
        shutil.copyfile(PROFILE / "layout.template.json", output / "config.json.ui.json")
    (output / "videos.txt").write_text("\n".join(rel(v) for v in videos) + "\n")
    write(output / "demo.json", {"assets": rel(root), "rtsp_port": args.rtsp_port,
          "dashboard_port": args.dashboard_port, "cpu_name": args.cpu_name})
    write(output / "config.json", config)
    print(output / "config.json")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    exp = commands.add_parser("export", help="Copy runtime model assets from an existing MobileCLIP demo")
    exp.add_argument("--config", type=Path, required=True)
    exp.add_argument("--videos-list", type=Path, required=True)
    exp.add_argument("--output", type=Path, required=True)
    exp.add_argument("--include-videos", action="store_true")
    conf = commands.add_parser("configure", help="Create editable settings for this machine")
    conf.add_argument("--assets", type=Path, required=True)
    conf.add_argument("--videos", type=Path, required=True)
    conf.add_argument("--output", type=Path, required=True)
    conf.add_argument("--backend", choices=["gpu", "npu", "gpu+npu"], default="npu")
    conf.add_argument("--venv", type=Path)
    conf.add_argument("--rtsp-port", type=int, default=18570)
    conf.add_argument("--dashboard-port", type=int, default=18572)
    conf.add_argument("--cpu-name", default="")
    conf.add_argument("--force", action="store_true")
    ref = commands.add_parser("refresh", help="Re-seal the export inventory after preparing models on the target")
    ref.add_argument("--assets", type=Path, required=True)
    chk = commands.add_parser("check", help="Verify files only; does not initialize GPU/NPU or start servers")
    chk.add_argument("--runtime", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == "export":
            export(args)
        elif args.command == "configure":
            configure(args)
        elif args.command == "refresh":
            refresh(args.assets.resolve())
        else:
            check(args.runtime.resolve())
    except (ValueError, KeyError, TypeError, OSError) as e:
        parser.exit(1, f"{e}\n")


if __name__ == "__main__":
    main()
