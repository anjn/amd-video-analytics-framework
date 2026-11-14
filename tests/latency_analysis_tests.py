import contextlib,csv,io,json,tempfile,unittest
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts/latency'))
from analyze_trial import analyze
from validate_trial import latency_gates
from validate_progress import validate_progress
class AnalysisTest(unittest.TestCase):
    def test_shutdown_rtp_retirement_cannot_hide_a_complete_au(self):
        for complete,post_stop in [(False,True),(True,True),(False,False)]:
            with tempfile.TemporaryDirectory() as temp:
                root=Path(temp);(root/'diagnostics').mkdir()
                events=[(1,'controller',0,'shutdown_requested'),(2,'cam0',1,'input')] if post_stop else [(1,'cam0',1,'input'),(2,'controller',0,'shutdown_requested')]
                if complete:events.append((3,'cam0',1,'access_unit'))
                events.append((4,'cam0',1,'rtp_assembly_retired'))
                with (root/'diagnostics/events.csv').open('w',newline='') as out:
                    writer=csv.writer(out);writer.writerow(['ns','source','generation','sequence','event','value','value2','pts'])
                    for ns,source,seq,event in events:writer.writerow([ns*1000000,source,1,seq,event,0,0,-1])
                (root/'diagnostics/diagnostics-summary.json').write_text(json.dumps(dict(accepted=len(events),written=len(events),queue_lost=0,limit_lost=0,pending=0,write_failed=False)))
                with contextlib.redirect_stdout(io.StringIO()):result=analyze(root)
                self.assertTrue(result['diagnostic_integrity_valid'])
                self.assertEqual(result['retirements_before_shutdown'],0)
                self.assertEqual(result['incomplete'][0]['kind'],'input_not_submitted' if complete or not post_stop else 'partial_rtp_retired')
                self.assertEqual(result['events'].get('invalid_rtp_retirement',0),int(complete))
                self.assertEqual(result['events'].get('pre_shutdown_rtp_unassembled',0),int(not post_stop))
    def test_complete_au_flush_requires_post_stop_input_and_explicit_closure(self):
        for post_stop,closed,complete,closure_after in [(True,True,True,True),(False,True,True,True),(True,False,True,True),(True,True,False,True),(True,True,True,False)]:
            with self.subTest(post_stop=post_stop,closed=closed,complete=complete,closure_after=closure_after),tempfile.TemporaryDirectory() as temp:
                root=Path(temp);(root/'diagnostics').mkdir()
                events=[(1 if not post_stop else 6,'cam0',1,'input'),(5,'controller',0,'shutdown_requested')]
                if complete:events.append((7,'cam0',1,'access_unit'))
                if closed:events.append((8 if closure_after else 6,'cam0',1,'receive_au_closed'))
                events.sort()
                with (root/'diagnostics/events.csv').open('w',newline='') as out:
                    writer=csv.writer(out);writer.writerow(['ns','source','generation','sequence','event','value','value2','pts'])
                    for ns,source,seq,event in events:writer.writerow([ns*1000000,source,1,seq,event,0,0,-1])
                (root/'diagnostics/diagnostics-summary.json').write_text(json.dumps(dict(accepted=len(events),written=len(events),queue_lost=0,limit_lost=0,pending=0,write_failed=False)))
                with contextlib.redirect_stdout(io.StringIO()):result=analyze(root)
                self.assertTrue(result['diagnostic_integrity_valid'])
                expected='shutdown_au_retired' if post_stop and closed and complete and closure_after else 'input_not_submitted'
                self.assertEqual(result['incomplete'][0]['kind'],expected)

    def test_epoch_identity_and_partial_frames(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);(root/'diagnostics').mkdir()
            with (root/'diagnostics/events.csv').open('w',newline='') as out:
                writer=csv.writer(out,escapechar='\\',doublequote=False)
                writer.writerow(['ns','source','generation','sequence','event','value','value2','pts'])
                def emit(ns,gen,seq,event,pts):writer.writerow([ns,'cam0',gen,seq,event,0,0,pts])
                emit(1000000,1,1,'input',100);emit(2000000,1,1,'decode_submit',100)
                emit(3000000,1,1,'decode_output',100);emit(3000001,1,1,'decode_output',100)
                emit(4000000,2,1,'input',0);emit(5000000,2,1,'decode_submit',0)
                emit(9000000,2,1,'decode_output',0)
                emit(10000000,2,2,'decode_submit',1)
                emit(11000000,2,3,'decode_output',2)
                emit(12000000,1,1,'pts',100)
                emit(13000000,1,2,'pts',90)
                emit(14000000,2,1,'pts',0)
            (root/'diagnostics/diagnostics-summary.json').write_text(json.dumps(
                dict(accepted=12,written=12,queue_lost=0,limit_lost=0,pending=0,write_failed=False)))
            with contextlib.redirect_stdout(io.StringIO()):result=analyze(root)
            metric=result['minutes'][0]['metrics']['input_decode']
            self.assertEqual(metric['count'],2);self.assertEqual(metric['p99'],5)
            self.assertEqual(result['events']['output_without_input_identity'],1)
            self.assertEqual(next(x['count'] for x in result['incomplete'] if x['kind']=='submitted_no_output'),1)
            self.assertTrue(result['diagnostic_integrity_valid'])
            self.assertEqual(result['records_read'],12)
            self.assertEqual(result['events']['pts_backwards_observed'],1)
            self.assertEqual(result['generations']['cam0'],[1,2])
            summary=root/'diagnostics/diagnostics-summary.json'
            data=json.loads(summary.read_text());data['queue_lost']=1;summary.write_text(json.dumps(data))
            with contextlib.redirect_stdout(io.StringIO()):result=analyze(root)
            self.assertFalse(result['diagnostic_integrity_valid'])
    def test_controlled_shutdown_dispositions_are_explicit(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);(root/'diagnostics').mkdir()
            events=[(1,'cam0',1,'input'),(2,'cam0',1,'decode_submit'),
                    (3,'cam0',1,'decode_output'),(4,'cam0',2,'input'),
                    (5,'controller',0,'shutdown_requested'),
                    (6,'cam0',1,'shutdown_latest_retired'),(7,'cam0',2,'ingest_retired')]
            with (root/'diagnostics/events.csv').open('w',newline='') as out:
                writer=csv.writer(out);writer.writerow(['ns','source','generation','sequence','event','value','value2','pts'])
                for ns,source,seq,event in events:writer.writerow([ns*1000000,source,1,seq,event,0,0,-1])
            (root/'diagnostics/diagnostics-summary.json').write_text(json.dumps(dict(accepted=7,written=7,queue_lost=0,limit_lost=0,pending=0,write_failed=False)))
            with contextlib.redirect_stdout(io.StringIO()):r=analyze(root)
            self.assertEqual(r['retirements_before_shutdown'],0)
            dispositions={x['kind']:x['count'] for x in r['incomplete']}
            self.assertEqual(dispositions,{'ingest_retired':1,'shutdown_retired':1})

    def test_preview_not_requested_is_distinct_from_unknown_loss(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);(root/'diagnostics').mkdir()
            events=[(1,1,'input'),(2,1,'decode_submit'),(3,1,'decode_output'),(4,1,'preview_not_requested'),
                    (5,2,'input'),(6,2,'decode_submit'),(7,2,'decode_output')]
            with (root/'diagnostics/events.csv').open('w',newline='') as out:
                w=csv.writer(out);w.writerow(['ns','source','generation','sequence','event','value','value2','pts'])
                for ns,seq,event in events:w.writerow([ns*1000000,'cam0',1,seq,event,2,0,-1])
            (root/'diagnostics/diagnostics-summary.json').write_text(json.dumps(dict(accepted=7,written=7,queue_lost=0,limit_lost=0,pending=0,write_failed=False)))
            with contextlib.redirect_stdout(io.StringIO()):r=analyze(root)
            self.assertEqual({v['kind']:v['count'] for v in r['incomplete']},{'preview_not_requested':1,'unclassified_not_drawn':1})
            self.assertEqual(r['unclassified_examples'][0]['sequence'],2)

class GateTest(unittest.TestCase):
    def fixture(self):
        return dict(stages={'cam0':{'decode_output':100}},
                    baseline_5_10={'cam0':{'input_decode':dict(count=10,p99=20,maximum=40)}},
                    stage_ranges_ns={'cam0':{'decode_output':[1000000000,3601000000000]}},
                    minutes=[dict(source='cam0',minute=10,metrics={'input_decode':dict(count=1800,p99=53,maximum=106)})],
                    diagnostic_integrity_valid=True,events={},incomplete=[])
    def test_threshold_and_actual_coverage(self):
        r=self.fixture();self.assertTrue(latency_gates(r)['latency_and_reconciliation_passed'])
        r['minutes'][0]['metrics']['input_decode']['p99']=54
        self.assertFalse(latency_gates(r)['latency_and_reconciliation_passed'])
        r=self.fixture();r['stage_ranges_ns']['cam0']['decode_output'][1]-=1000000000
        self.assertFalse(latency_gates(r)['latency_and_reconciliation_passed'])
    def test_loss_and_unfinished_are_failures(self):
        r=self.fixture();r['diagnostic_integrity_valid']=False
        self.assertFalse(latency_gates(r)['latency_and_reconciliation_passed'])
        r=self.fixture();r['incomplete']=[dict(source='cam0',kind='ai_submitted_no_completion',count=1)]
        self.assertFalse(latency_gates(r)['latency_and_reconciliation_passed'])
        r=self.fixture();r['incomplete']=[dict(source='cam15',kind='background_latest_released',count=1)]
        self.assertFalse(latency_gates(r)['latency_and_reconciliation_passed'])
    def test_successful_decode_cannot_hide_recording_input_loss(self):
        r=self.fixture();r['events']['ingest_discarded']=1
        self.assertFalse(latency_gates(r)['latency_and_reconciliation_passed'])
        r=self.fixture();r['events']['decode_preroll']=1
        self.assertFalse(latency_gates(r)['latency_and_reconciliation_passed'])
    def test_drain_or_retirement_errors_cannot_pass(self):
        for name in ['invalid_rtp_retirement','decoder_drain_failed','receive_drain_failed','pre_shutdown_rtp_unassembled']:
            r=self.fixture();r['events'][name]=1
            self.assertFalse(latency_gates(r)['latency_and_reconciliation_passed'])
class ProgressTest(unittest.TestCase):
    def run_fixture(self,stop_reid=False,error=False,resource_missing=False,end=180):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);(root/'diagnostics').mkdir()
            (root/'manifest.json').write_text(json.dumps({'start_monotonic_ns':0}))
            rows=[]
            stages=['YOLO inference','ReID inference + transfer','Image embedding NPU','PNG + DB save']
            for elapsed in range(0,end+1,5):
                ai=[dict(stage=stage,camera='',completed=min(elapsed,125) if stop_reid and stage=='ReID inference + transfer' else elapsed) for stage in stages]
                channels=[dict(id='cam0',error='',slot=0,decoded=elapsed*30,display_fps=30)]
                if resource_missing:channels.append(dict(id='resource-monitor',error='',slot=15,decoded=0,display_fps=0))
                rows.append(dict(ns=elapsed*1000000000,ai=ai,channels=channels,fatal='',search={'tracking_error':'VPE not observed' if error and elapsed>=125 else ''}))
            (root/'diagnostics/status.jsonl').write_text('\n'.join(json.dumps(r) for r in rows)+'\n')
            return validate_progress(root)
    def test_yolo_continuing_does_not_hide_reid_stop(self):
        self.assertTrue(self.run_fixture()['progress_passed'])
        result=self.run_fixture(stop_reid=True)
        self.assertFalse(result['progress_passed'])
        self.assertEqual(result['failures'][0]['stage'],'ReID inference + transfer')
    def test_missing_resource_tile_cannot_pass_short_pilot(self):
        result=self.run_fixture(resource_missing=True,end=115)
        self.assertFalse(result['progress_passed'])
        self.assertTrue(any(v['stage']=='resource-monitor' for v in result['failures']))
    def test_error_is_not_erased_by_later_progress(self):
        self.assertFalse(self.run_fixture(error=True)['progress_passed'])
if __name__=='__main__':unittest.main()
