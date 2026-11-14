import sys,unittest,csv,json,tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts/latency'))
from size_fifo import arrival_envelope,pts_summary,size_fifo
class FifoSizing(unittest.TestCase):
    def test_constant_rate_has_one_frame_margin(self):
        r=arrival_envelope([round(i*1e9/30) for i in range(100)])
        self.assertEqual(r['initial_delay_frames'],1)
        self.assertEqual(r['capacity_frames_for_observed_arrivals'],2)
    def test_stall_and_catchup_need_more_than_largest_single_gap(self):
        times=[round(i*1e9/30) for i in range(100)]
        for i in range(5,8):times[i]=max(times[i],times[5]+80_000_000 if i==5 else times[i-1]+1000)
        r=arrival_envelope(times)
        self.assertEqual(r['initial_delay_frames'],4)
        self.assertGreaterEqual(r['capacity_frames_for_observed_arrivals'],5)
    def test_slow_producer_is_visible_as_drift(self):
        r=arrival_envelope([i*40_000_000 for i in range(100)])
        self.assertAlmostEqual(r['observed_output_fps'],25)
        self.assertGreater(r['ending_phase_ms'],600)
    def test_clock_reversal_and_pts_gaps_are_not_hidden(self):
        with self.assertRaises(ValueError):arrival_envelope([10,9])
        r=pts_summary([0,33_333_333,100_000_000,90_000_000,90_000_000])
        self.assertEqual(r['intervals_above_1_5_periods'],1)
        self.assertEqual(r['backwards'],1);self.assertEqual(r['duplicates'],1)
    def test_csv_modes_warmup_and_integrity(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);(root/'diagnostics').mkdir();rows=[]
            rows.append([1,'controller',0,0,'warmup',0,0,-1])
            for i in range(10):
                ns=1_000_000_000+round(i*1e9/30);pts=round(i*1e9/30)
                rows.extend([[ns,'cam0',1,i+1,'decode_output',0,0,-1],
                             [ns,'cam0',1,i+1,'pts',4,0,pts],
                             [ns+1_000_000,'cam0',1,i+1,'handoff',0,0,-1]])
            with (root/'diagnostics/events.csv').open('w',newline='') as f:
                w=csv.writer(f);w.writerow(['ns','source','generation','sequence','event','value','value2','pts']);w.writerows(rows)
            integrity=dict(accepted=len(rows),written=len(rows),queue_lost=0,limit_lost=0,pending=0,write_failed=False)
            summary=root/'diagnostics/diagnostics-summary.json';summary.write_text(json.dumps(integrity))
            r=size_fifo(root,skip_seconds=0)
            self.assertEqual(r['records_read'],31);self.assertEqual(len(r['streams']),2)
            self.assertEqual(r['streams'][0]['pts']['samples'],10)
            self.assertEqual(r['streams'][0]['arrival']['capacity_frames_for_observed_arrivals'],2)
            with self.assertRaisesRegex(ValueError,'No eligible'):size_fifo(root)
            with (root/'diagnostics/events.csv').open('a',newline='') as f:
                csv.writer(f).writerow(rows[-1])
            integrity['accepted']+=1;integrity['written']+=1;summary.write_text(json.dumps(integrity))
            with self.assertRaisesRegex(ValueError,'Duplicate'):size_fifo(root,skip_seconds=0)
            integrity['queue_lost']=1;summary.write_text(json.dumps(integrity))
            with self.assertRaisesRegex(ValueError,'loss'):size_fifo(root,skip_seconds=0)
if __name__=='__main__':unittest.main()
