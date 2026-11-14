import importlib.util,unittest
from pathlib import Path
path=Path(__file__).resolve().parents[1]/'scripts/latency/validate_presentations.py'
spec=importlib.util.spec_from_file_location('physical',path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
def row(e,t,seq=0,src='cam0',pts=100):
 return dict(event=e,ns=t,sequence=seq,source=src,generation=1,pts=pts,value=0,value2=1)
def normal():
 return [row('presentation_mode',0,src='presentation'),row('decode_output',10,1),row('fifo_enter',11,1),row('draw',12,1),row('swap',13,1),row('presentation_commit_presented',14,1),row('presented',14,1),row('shutdown_requested',100,src='controller')]
class PhysicalTests(unittest.TestCase):
 def check_pass(self,rows):self.assertTrue(m.reconcile(rows,['cam0'])['physical_frames_passed'])
 def check_fail(self,rows,why):self.assertIn(why,m.reconcile(rows,['cam0'])['failures'])
 def test_normal(self):self.check_pass(normal())
 def test_swap_not_physical(self):self.check_fail([r for r in normal() if r['event']!='presented'],'decoded_frame_not_presented')
 def test_discarded_commit_not_lost_frame(self):
  rows=normal();rows.insert(4,row('presentation_commit_discarded',12,1));self.check_pass(rows)
 def test_error_never_excused(self):
  rows=normal();rows.append(row('presentation_error',15,src='presentation'));self.check_fail(rows,'presentation_error')
 def test_recent_shutdown_retired(self):
  rows=normal()+[row('decode_output',90,2,pts=200),row('fifo_enter',91,2),row('shutdown_fifo_retired',101,2)];self.check_pass(rows)
 def test_stale_shutdown_cannot_hide_old_loss(self):
  rows=normal();rows[-1]['ns']=70_000_000_000
  rows.extend([row('decode_output',200,2),row('draw',201,2),row('shutdown_present_retired',70_000_000_010,2)])
  self.check_fail(rows,'decoded_frame_not_presented')
 def test_missing_mode(self):self.check_fail(normal()[1:],'missing_or_duplicate_physical_mode')
 def test_duplicate_unique_frame(self):
  rows=normal();rows.insert(-1,row('presented',15,1));self.check_fail(rows,'duplicate_unique_presentation')
 def test_startup_separate(self):
  rows=[row('decode_output',1,2),row('preview_not_requested',2,2)]+normal();self.check_pass(rows)
 def test_unannounced_suspension_fails(self):
  rows=normal()+[row('decode_output',20,2),row('preview_not_requested',21,2)];self.check_fail(rows,'decoded_frame_not_presented')
 def test_pts_order(self):
  rows=normal();extra=[row(e,t+20,2,pts=50) for e,t in [('decode_output',10),('fifo_enter',11),('draw',12),('swap',13),('presentation_commit_presented',14),('presented',14)]]
  self.check_fail(rows[:-1]+extra+rows[-1:],'presented_pts_not_increasing')
 def test_physical_timestamp_mismatch(self):
  rows=normal();next(r for r in rows if r['event']=='presented')['ns']=15
  self.check_fail(rows,'physical_timestamp_mismatch')
 def test_retirement_before_shutdown_fails(self):
  rows=normal()+[row('decode_output',90,2),row('fifo_enter',91,2),row('shutdown_fifo_retired',99,2)]
  self.check_fail(rows,'decoded_frame_not_presented')
 def test_record_loss_fails(self):
  import tempfile,csv,json
  with tempfile.TemporaryDirectory() as tmp:
   p=Path(tmp);(p/'diagnostics').mkdir();rows=normal()
   with (p/'diagnostics/events.csv').open('w',newline='') as f:
    writer=csv.DictWriter(f,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
   summary={'accepted':len(rows),'written':len(rows),'queue_lost':1,'limit_lost':0,'pending':0,'write_failed':False}
   (p/'diagnostics/diagnostics-summary.json').write_text(json.dumps(summary))
   self.assertFalse(m.validate(p,['cam0'])['physical_frames_passed'])
if __name__=='__main__':unittest.main()
