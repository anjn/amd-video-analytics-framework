#!/usr/bin/env python3
"""Portability contracts requiring neither GPU hardware nor model downloads."""
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import tarfile
import io
import shutil
import socket
import subprocess
import unittest
from types import SimpleNamespace
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT / "scripts"))
import host
import bootstrap
import prepare_demo


class SetupContracts(unittest.TestCase):
    def test_explicit_bad_sdk_never_falls_back(self):
        with tempfile.TemporaryDirectory() as d:
            good=Path(d)/"good";good.mkdir();(good/"marker").touch()
            with self.assertRaises(ValueError):host.choose(str(Path(d)/"bad"),[str(good)],"marker")

    def test_ambiguous_sdk_requires_selection(self):
        with tempfile.TemporaryDirectory() as d:
            roots=[Path(d)/"a",Path(d)/"b"]
            for r in roots:r.mkdir();(r/"marker").touch()
            with self.assertRaises(ValueError):host.choose(None,[str(r) for r in roots],"marker")

    def test_symlink_alias_is_one_sdk(self):
        with tempfile.TemporaryDirectory() as d:
            r=Path(d)/"real";r.mkdir();(r/"marker").touch()
            link=Path(d)/"link";link.symlink_to(r,target_is_directory=True)
            self.assertEqual(host.choose(None,[str(r),str(link)],"marker"),r.resolve())

    def test_inherited_loader_pollution_removed(self):
        with patch.dict(os.environ,{"LD_LIBRARY_PATH":"/torch/lib","PYTHONPATH":"/other","HSA_OVERRIDE_GFX_VERSION":"11.5.0"}):
            clean=host.clean_env()
            self.assertNotIn("LD_LIBRARY_PATH",clean);self.assertNotIn("PYTHONPATH",clean);self.assertNotIn("HSA_OVERRIDE_GFX_VERSION",clean)

    def test_sdk_version_is_not_overwritten_by_tool_version(self):
        with tempfile.TemporaryDirectory() as d:
            rocm=Path(d)/"rocm";mx=Path(d)/"mx"
            for root,names in [(rocm,[".info/version","include/hip/hip_runtime.h","lib/libamdhip64.so","lib/libhiprtc.so","lib/libroctx64.so.4"]),
                               (mx,["lib/cmake/migraphx/migraphx-config-version.cmake","include/migraphx/migraphx.hpp","lib/libmigraphx_c.so","lib/migraphx/lib/libmigraphx_gpu.so"])]:
                for name in names:
                    f=root/name;f.parent.mkdir(parents=True,exist_ok=True);f.touch()
            (rocm/".info/version").write_text("10.1.0")
            (mx/"lib/cmake/migraphx/migraphx-config-version.cmake").write_text('set(PACKAGE_VERSION "2.18.0")')
            original=Path.read_text
            for os_version in ["24.04","26.04"]:
                def read(path,*args,**kwargs):
                    return f'ID=ubuntu\nVERSION_ID="{os_version}"\n' if str(path)=="/etc/os-release" else original(path,*args,**kwargs)
                def command(cmd,**kwargs):
                    return "libraries resolved" if cmd[0]=="ldd" else f"{cmd[0]} 1.96.0"
                with patch.dict(os.environ,{"AMDVA_ROCM_ROOT":str(rocm),"AMDVA_MIGRAPHX_ROOT":str(mx)}),patch.object(Path,"read_text",read),patch("subprocess.check_output",side_effect=command),patch("subprocess.run",return_value=SimpleNamespace(returncode=0,stdout="installed 1")),patch("shutil.which",side_effect=lambda name:"/usr/bin/"+name):
                    result=host.inspect(devices=False)
                    self.assertEqual(result["rocm"],"10.1.0")
                    self.assertEqual(result["migraphx"],"2.18.0")

    def test_wrong_rocm_version_is_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            rocm=Path(d)/"rocm";(rocm/".info").mkdir(parents=True);(rocm/".info/version").write_text("7.2.0")
            original=Path.read_text
            def read(path,*args,**kwargs):
                return 'ID=ubuntu\nVERSION_ID="24.04"\n' if str(path)=="/etc/os-release" else original(path,*args,**kwargs)
            with patch.dict(os.environ,{"AMDVA_ROCM_ROOT":str(rocm)}),patch.object(Path,"read_text",read):
                with self.assertRaisesRegex(ValueError,"required 10.1.0"):
                    host.inspect(devices=False,build=False)

    def test_wrong_os_is_rejected_before_sdk_selection(self):
        with patch.object(Path, "read_text", return_value='ID=debian\nVERSION_ID="13"\n'):
            with self.assertRaisesRegex(ValueError, "Supported OS"):
                host.inspect(devices=False)

    def test_corrupt_download_not_published(self):
        with tempfile.TemporaryDirectory() as d:
            target=Path(d)/"model"
            def corrupt(url,dest):Path(dest).write_bytes(b"wrong")
            with patch("urllib.request.urlretrieve",side_effect=corrupt):
                with self.assertRaises(RuntimeError):bootstrap.download("https://invalid/model",target,"0"*64)
            self.assertFalse(target.exists())

    def videos(self,d,count=16):
        paths=[]
        for i in range(count):
            path=Path(d)/f"camera {i}.mp4";path.touch();paths.append(path.name)
        listing=Path(d)/"videos.txt";listing.write_text("\n".join(paths));return listing

    def stream(self,**change):
        s=dict(codec_name="hevc",width=2560,height=1440,r_frame_rate="30/1",avg_frame_rate="30/1");s.update(change)
        return json.dumps({"streams":[s]}).encode()

    def test_partial_source_extraction_is_replaced_only_when_complete(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);target=root/"source";target.mkdir();(target/"partial").touch()
            archive=root/"source.tar"
            with tarfile.open(archive,"w") as tar:
                data=b"complete";info=tarfile.TarInfo("source/configure");info.size=len(data);tar.addfile(info,io.BytesIO(data))
            self.assertTrue(bootstrap.extract_source(archive,target,"digest"))
            self.assertEqual((target/"configure").read_bytes(),b"complete")
            self.assertFalse(bootstrap.extract_source(archive,target,"digest"))
            self.assertEqual(len(list(root.glob("source.unverified-*"))),1)

    def test_unsafe_archive_cannot_replace_existing_sources(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);target=root/"source";target.mkdir();(target/"original").touch()
            archive=root/"unsafe.tar"
            with tarfile.open(archive,"w") as tar:
                info=tarfile.TarInfo("../"+root.name+"-escape");info.size=1;tar.addfile(info,io.BytesIO(b"x"))
            with self.assertRaises(tarfile.FilterError):bootstrap.extract_source(archive,target,"digest")
            self.assertTrue((target/"original").exists())
            self.assertFalse((root.parent/(root.name+"-escape")).exists())

    def ort_fixture(self, d):
        root=Path(d);header=root/"onnxruntime/include/onnxruntime/onnxruntime_cxx_api.h"
        header.parent.mkdir(parents=True);header.touch();return root

    def test_gpu_only_ort_does_not_probe_npu(self):
        with tempfile.TemporaryDirectory() as d:
            venv=self.ort_fixture(d)
            response=json.dumps({"version":"1.30.0","providers":["CPUExecutionProvider"]})
            with patch("subprocess.check_output",return_value=response) as command,patch("resource.getrlimit",side_effect=AssertionError("must not inspect NPU memlock")):
                result=host.inspect_ort(venv,"26.04",npu=False)
                self.assertEqual(result["onnxruntime"],"1.30.0")
                self.assertNotIn("xrt",result)
                self.assertEqual(command.call_count,1)

    def test_npu_still_requires_vitis_provider(self):
        with tempfile.TemporaryDirectory() as d:
            venv=self.ort_fixture(d)
            response=json.dumps({"version":"1.30.0","providers":["CPUExecutionProvider"]})
            with patch("subprocess.check_output",return_value=response):
                with self.assertRaisesRegex(ValueError,"VitisAI"):host.inspect_ort(venv,"26.04",npu=True)

    def test_npu_rejects_low_memlock(self):
        with tempfile.TemporaryDirectory() as d:
            venv=self.ort_fixture(d)
            response=json.dumps({"version":"1.30.0","providers":["CPUExecutionProvider","VitisAIExecutionProvider"]})
            with patch("subprocess.check_output",return_value=response),patch("resource.getrlimit",return_value=(8192,8192)):
                with self.assertRaisesRegex(ValueError,"memlock"):host.inspect_ort(venv,"26.04",npu=True)

    def test_ort_cpu_text_provider_is_required(self):
        with tempfile.TemporaryDirectory() as d:
            venv=self.ort_fixture(d)
            response=json.dumps({"version":"1.30.0","providers":[]})
            with patch("subprocess.check_output",return_value=response):
                with self.assertRaisesRegex(ValueError,"CPU text"):host.inspect_ort(venv,"26.04",npu=False)

    def test_launcher_preserves_previous_logs_and_check_is_read_only(self):
        with tempfile.TemporaryDirectory(prefix="demo logs ") as d:
            root=Path(d);profile=root/"examples/nvr/demo/1440p";profile.mkdir(parents=True)
            shutil.copyfile(ROOT/"examples/nvr/demo/1440p/run.sh",profile/"run.sh")
            (profile/"manage.py").write_text("")
            def executable(name,text):
                path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text);path.chmod(0o755)
            executable("scripts/env.sh","#!/bin/bash\n")
            for name in ["build/examples/rtsp-files/amdva-rtsp-files","build/examples/resource-dashboard/amdva-dashboard"]:
                executable(name,"#!/usr/bin/python3\nimport time\ntime.sleep(60)\n")
            executable("build/examples/nvr/amdva-nvr","#!/bin/bash\nexit 0\n")
            executable("examples/nvr/scripts/run.sh","#!/bin/bash\necho new-run\n")
            runtime=root/"runtime";logs=runtime/"logs";logs.mkdir(parents=True);(logs/"previous.log").write_text("previous evidence")
            sockets=[];ports=[]
            for _ in range(2):
                sock=socket.socket();sock.bind(("127.0.0.1",0));sockets.append(sock);ports.append(sock.getsockname()[1])
            for sock in sockets:sock.close()
            (runtime/"demo.json").write_text(json.dumps({"rtsp_port":ports[0],"dashboard_port":ports[1],"cpu_name":""}))
            env=dict(os.environ,QT_QPA_PLATFORM="offscreen");env.pop("LD_PRELOAD",None);env.pop("AMDVA_COPY_AUDIT",None)
            subprocess.run(["bash",str(profile/"run.sh"),"--check",str(runtime)],check=True,env=env,capture_output=True,timeout=15)
            self.assertEqual((logs/"previous.log").read_text(),"previous evidence")
            self.assertEqual(list(runtime.glob("logs.*")),[])
            subprocess.run(["bash",str(profile/"run.sh"),str(runtime)],check=True,env=env,capture_output=True,timeout=15)
            archived=list(runtime.glob("logs.*"));self.assertEqual(len(archived),1)
            self.assertEqual((archived[0]/"previous.log").read_text(),"previous evidence")
            self.assertIn("new-run",(logs/"nvr.log").read_text())

    def test_video_count_rejected_before_probe(self):
        with tempfile.TemporaryDirectory() as d,patch("subprocess.check_output") as probe:
            with self.assertRaises(ValueError):prepare_demo.validate_videos(self.videos(d,15))
            probe.assert_not_called()

    def test_video_shape_rate_codec_checked(self):
        with tempfile.TemporaryDirectory() as d:
            listing=self.videos(d)
            for changed in [dict(codec_name="h264"),dict(width=1920),dict(avg_frame_rate="30000/1001"),dict(r_frame_rate="60/1")]:
                with self.subTest(changed=changed),patch("subprocess.check_output",return_value=self.stream(**changed)):
                    with self.assertRaises(ValueError):prepare_demo.validate_videos(listing)

    def test_video_paths_with_spaces_supported(self):
        with tempfile.TemporaryDirectory() as d,patch("subprocess.check_output",return_value=self.stream()):
            self.assertEqual(len(prepare_demo.validate_videos(self.videos(d))),16)


if __name__=="__main__":unittest.main()
