import json,sys,tempfile,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts/latency'))
from validate_audits import validate_audits
class AuditValidation(unittest.TestCase):
    def trial(self,root):
        d=Path(root);(d/'diagnostics').mkdir()
        def save(name,value):(d/name).write_text(json.dumps(value))
        save('manifest.json',dict(exit_reason='duration',returncode=0,diagnostic_launcher=False))
        save('diagnostics/copy-audit-summary.json',dict(requested=True,active_start_ns=1,active_end_ns=3600_000_000_001,violations=0))
        save('diagnostics/vpe-audit-summary.json',dict(method='binary-syncobj-fence',scopes=200,completed=200,submissions=200,failed=0,aborted=0,rejected_submissions=0,foreign_operations=0,active_scope=0))
        save('diagnostics/status.jsonl',dict(ns=100,copy_audit=dict(active=True,start_ns=1,violations=0,copies=0),vpe_audit=dict(available=True,failed=0,aborted=0,rejected=0,foreign=0)))
        return d,save
    def test_complete_active_audits(self):
        with tempfile.TemporaryDirectory() as root:
            d,_=self.trial(root);self.assertTrue(validate_audits(d)['audits_passed'])
    def test_preload_alone_is_not_active_proof(self):
        with tempfile.TemporaryDirectory() as root:
            d,save=self.trial(root);save('diagnostics/copy-audit-summary.json',{});self.assertFalse(validate_audits(d)['audits_passed'])
    def test_partial_fence_proof_and_debug_launcher_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            d,save=self.trial(root);save('manifest.json',dict(exit_reason='duration',returncode=0,diagnostic_launcher=True));save('diagnostics/vpe-audit-summary.json',dict(method='binary-syncobj-fence',scopes=200,completed=199))
            r=validate_audits(d);self.assertFalse(r['audits_passed']);self.assertGreater(len(r['failures']),1)
if __name__=='__main__':unittest.main()
