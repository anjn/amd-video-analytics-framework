"""Resolve only this checkout's auditors; keep preloaded VPE binaries immutable."""
import hashlib,os,sys,tempfile
from pathlib import Path

def snapshot(root,library):
    data=library.read_bytes()
    if len(data)<64 or data[:5]!=b'\x7fELF\x02':
        raise RuntimeError(f'Invalid built audit library: {library}')
    digest=hashlib.sha256(data).hexdigest();cache=root/'.deps/audit-runtime';cache.mkdir(parents=True,exist_ok=True)
    destination=cache/(digest+'.so')
    if destination.exists():
        if destination.is_symlink() or hashlib.sha256(destination.read_bytes()).hexdigest()!=digest:
            raise RuntimeError(f'Corrupt audit runtime snapshot: {destination}')
        return destination
    descriptor,temporary=tempfile.mkstemp(prefix='publish-',dir=cache)
    try:
        with os.fdopen(descriptor,'wb') as output:output.write(data)
        os.chmod(temporary,0o644)
        try:os.link(temporary,destination)
        except FileExistsError:
            if destination.is_symlink() or hashlib.sha256(destination.read_bytes()).hexdigest()!=digest:raise RuntimeError('Concurrent corrupt audit snapshot')
    finally:Path(temporary).unlink(missing_ok=True)
    return destination

def resolve(root):
    vpe=next((p for p in [root/'build/libamdva_vpe_audit.so',root/'.deps/prefix/lib/libamdva_vpe_audit.so'] if p.is_file()),None)
    copy=next((p for p in [root/'build/src/gst/libamdva_copy_audit.so',root/'.deps/prefix/lib/libamdva_copy_audit.so'] if p.is_file()),None)
    return str(snapshot(root,vpe)) if vpe else '',str(copy) if copy else ''

if __name__=='__main__':
    try:
        for path in resolve(Path(__file__).resolve().parents[1]):print(path)
    except (OSError,RuntimeError) as error:sys.exit(str(error))
