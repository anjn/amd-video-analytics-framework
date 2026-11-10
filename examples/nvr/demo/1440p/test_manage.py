#!/usr/bin/env python3
"""File-only deployment contract; fake model bytes never execute on GPU/NPU."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

PROFILE = Path(__file__).resolve().parent


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class DemoPortability(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="amdva demo ")
        self.root = Path(self.temp.name)
        self.source = self.root / "original checkout"
        self.source.mkdir()
        config = json.loads((PROFILE / "config.template.json").read_text())
        for key, name, batch in [("model", "detector-b4.mxr", 4),
                                 ("single_model", "detector-b1.mxr", 1),
                                 ("reid", "reid-b4.mxr", 4)]:
            path = self.source / name
            path.write_bytes(name.encode())
            save(Path(str(path) + ".manifest.json"),
                 {"sha256": sha(path), "batch": batch, "offload_copy": False, "gpu": "gfx1150"})
            if key == "reid":
                config["search"]["reid_model"] = name
            else:
                config[key] = name
        for name in ["model-info.json", "coco80.txt"]:
            shutil.copyfile(PROFILE / name, self.source / name)
        bundle = self.source / "mobileclip"
        bundle.mkdir()
        manifest = {}
        for name, suffix in [("image", ".onnx"), ("text", ".onnx"), ("tokenizer", ".json")]:
            path = bundle / (name + suffix)
            path.write_bytes(name.encode())
            manifest[name + "_sha256"] = sha(path)
        save(bundle / "manifest.json", manifest)
        artifacts = {}
        for batch in [1, 4]:
            path = bundle / f"gpu-device-b{batch}.mxr"
            path.write_bytes(f"GPU {batch}".encode())
            save(Path(str(path) + ".manifest.json"), {"sha256": sha(path)})
            for item in [path, Path(str(path) + ".manifest.json")]:
                artifacts[item.name] = sha(item)
        save(bundle / "gpu-device.json", {"image_sha256": manifest["image_sha256"], "artifacts": artifacts})
        save(bundle / "npu-config.json", {})
        save(bundle / "npu-cache/key/context.json", {})
        (bundle / "npu-cache/key/key.rai").write_bytes(b"cache")
        artifacts = {str(p.relative_to(bundle)): sha(p) for p in
                     [bundle / "npu-config.json", bundle / "npu-cache/key/context.json", bundle / "npu-cache/key/key.rai"]}
        save(bundle / "npu.json", {"cache_key": "key", "image_sha256": manifest["image_sha256"],
                                  "onnxruntime": "1.30.0", "artifacts": artifacts})
        config["search"]["mobileclip_bundle"] = "mobileclip"
        self.config = self.source / "config.json"
        save(self.config, config)
        self.videos = self.root / "videos with spaces"
        self.videos.mkdir()
        names = (PROFILE / "videos.template.txt").read_text().splitlines()
        for name in names:
            (self.videos / name).write_bytes(b"video fixture")
        self.video_list = self.source / "videos.txt"
        self.video_list.write_text("\n".join(os.path.relpath(self.videos / n, self.source) for n in names))
        self.assets = self.root / "export assets"
        self.runtime = self.root / "runtime moved checkout"
        self.venv = self.root / "Ryzen AI env"
        (self.venv / "bin").mkdir(parents=True)
        (self.venv / "bin/activate").touch()
        (self.venv / "onnxruntime/lib").mkdir(parents=True)

    def tearDown(self):
        self.temp.cleanup()

    def command(self, *args, ok=True):
        result = subprocess.run([sys.executable, str(PROFILE / "manage.py"), *map(str, args)],
                                text=True, capture_output=True)
        self.assertEqual(result.returncode == 0, ok, result.stdout + result.stderr)
        return result

    def export(self, *extra):
        self.command("export", "--config", self.config, "--videos-list", self.video_list,
                     "--output", self.assets, *extra)

    def configure(self, *extra, ok=True):
        return self.command("configure", "--assets", self.assets, "--videos", self.videos,
                            "--venv", self.venv, "--output", self.runtime,
                            "--rtsp-port", 18670, "--dashboard-port", 18672, *extra, ok=ok)

    def test_export_move_configure_and_check(self):
        self.export("--include-videos")
        moved = self.root / "another mount $ with spaces"
        shutil.move(self.assets, moved)
        self.assets = moved
        self.videos = moved / "videos"
        self.configure()
        self.command("check", "--runtime", self.runtime)
        config = json.loads((self.runtime / "config.json").read_text())
        self.assertFalse(Path(config["model"]).is_absolute())
        self.assertNotIn(str(self.source), json.dumps(config))
        self.assertEqual(len(config["sources"]), 17)
        self.assertEqual(sum(s["selected"] for s in config["sources"]), 16)
        self.assertEqual(config["sources"][0]["uri"], "rtsp://127.0.0.1:18670/cam0")
        self.assertEqual(config["sources"][-1]["uri"], "rtsp://127.0.0.1:18672/dashboard")
        self.assertEqual(config["recording_root"], "recordings")
        self.assertEqual(len((self.runtime / "videos.txt").read_text().splitlines()), 16)

    def test_tampered_asset_rejected_before_config_write(self):
        self.export()
        (self.assets / "detector-b4.mxr").write_bytes(b"changed")
        self.configure(ok=False)
        self.assertFalse((self.runtime / "config.json").exists())

    def test_no_overwrite_and_force_preserves_user_layout(self):
        self.export()
        self.configure()
        config = (self.runtime / "config.json").read_bytes()
        self.configure(ok=False)
        self.assertEqual((self.runtime / "config.json").read_bytes(), config)
        custom = {"layout": 1, "cells": ["cam2"], "version": 1}
        save(self.runtime / "config.json.ui.json", custom)
        self.configure("--force")
        self.assertEqual(json.loads((self.runtime / "config.json.ui.json").read_text()), custom)

    def test_missing_video_and_conflicting_ports_rejected(self):
        self.export()
        self.configure("--dashboard-port", 18670, ok=False)
        next(self.videos.glob("*.mp4")).unlink()
        self.configure(ok=False)
        self.assertFalse((self.runtime / "config.json").exists())

    def test_refresh_after_recompilation_requires_valid_sidecar(self):
        self.export()
        model = self.assets / "reid-b4.mxr"
        model.write_bytes(b"recompiled")
        self.command("refresh", "--assets", self.assets, ok=False)
        sidecar = Path(str(model) + ".manifest.json")
        meta = json.loads(sidecar.read_text())
        meta["sha256"] = sha(model)
        save(sidecar, meta)
        self.command("refresh", "--assets", self.assets)
        self.configure()

    def test_bundle_path_traversal_is_rejected(self):
        path = self.source / "mobileclip/gpu-device.json"
        meta = json.loads(path.read_text())
        meta["artifacts"]["../../outside"] = "bad"
        save(path, meta)
        self.command("export", "--config", self.config, "--videos-list", self.video_list,
                     "--output", self.assets, ok=False)
        self.assertFalse(self.assets.exists())


if __name__ == "__main__":
    unittest.main()
