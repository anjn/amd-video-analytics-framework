#!/usr/bin/env python3
"""Record the actual NPU and loaded driver; do not bless caches from other hosts."""
import argparse
import json
from pathlib import Path


def identity():
    devices=[]
    for pci in Path("/sys/bus/pci/devices").iterdir():
        try:
            vendor=(pci/"vendor").read_text().strip()
            device=(pci/"device").read_text().strip()
            revision=(pci/"revision").read_text().strip()
        except OSError:
            continue
        if vendor=="0x1022" and device=="0x17f0":
            devices.append(":".join([vendor,device,revision]))
    if len(devices)!=1:
        raise ValueError("Expected exactly one supported Strix Point/Halo NPU")
    return {"npu_device":devices[0],"npu_driver":Path("/sys/module/amdxdna/version").read_text().strip()}


def main():
    p=argparse.ArgumentParser();p.add_argument("action",choices=["check","seal"]);p.add_argument("bundle",type=Path);a=p.parse_args()
    current=identity();path=a.bundle / "npu.json"
    if a.action=="check":
        if (a.bundle/"npu-cache").exists():
            meta=json.loads(path.read_text())
            if any(meta.get(k)!=v for k,v in current.items()):
                raise ValueError("NPU cache environment mismatch; prepare a new output directory")
    else:
        meta=json.loads(path.read_text());meta.update(current)
        path.write_text(json.dumps(meta,indent=2)+"\n")


if __name__=="__main__":main()
