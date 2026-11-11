#!/usr/bin/env python3
"""Create the checksum-locked CPU export environment; never load Torch in the app."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[1]
venv=root / ".deps/model-export"
uv=root / ".deps/tools/bin/uv"
if not (venv/"bin/python").exists():
    subprocess.run([str(uv),"venv","--python","3.12",str(venv)],check=True)
subprocess.run([str(uv),"pip","sync","--python",str(venv/"bin/python"),"--require-hashes",
    "--index-url","https://pypi.org/simple","--extra-index-url","https://download.pytorch.org/whl/cpu",
    "--index-strategy","unsafe-best-match",str(root/"scripts/model-export.lock")],check=True)
