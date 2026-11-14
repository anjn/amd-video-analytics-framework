import concurrent.futures,hashlib,importlib.util,tempfile,unittest
from pathlib import Path
spec=importlib.util.spec_from_file_location('audit_env',Path(__file__).resolve().parents[1]/'scripts/audit_env.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
class AuditEnvironmentTest(unittest.TestCase):
    def library(self,root,suffix):
        file=root/'build/libamdva_vpe_audit.so';file.parent.mkdir(exist_ok=True)
        # Metadata-only fixture; never preloaded or executed.
        file.write_bytes(b'\x7fELF\x02'+bytes(64)+suffix);return file
    def test_fresh_checkout_and_immutable_rebuild(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);self.assertEqual(module.resolve(root),('',''))
            file=self.library(root,b'first');first=Path(module.resolve(root)[0]);original=first.read_bytes()
            self.library(root,b'second');second=Path(module.resolve(root)[0])
            self.assertNotEqual(first,second);self.assertEqual(first.read_bytes(),original)
            self.assertEqual(second.stem,hashlib.sha256(file.read_bytes()).hexdigest())
    def test_concurrent_publication_and_corruption(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);self.library(root,b'parallel')
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:paths=list(pool.map(lambda _:module.resolve(root)[0],range(16)))
            self.assertEqual(len(set(paths)),1);file=Path(paths[0]);file.chmod(0o644);file.write_bytes(b'corrupt')
            with self.assertRaisesRegex(RuntimeError,'Corrupt'):module.resolve(root)
    def test_invalid_build_artifact_stops(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);file=self.library(root,b'bad');file.write_bytes(b'truncated')
            with self.assertRaisesRegex(RuntimeError,'Invalid'):module.resolve(root)
if __name__=='__main__':unittest.main()
