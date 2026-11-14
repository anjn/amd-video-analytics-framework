import importlib.util,unittest
from pathlib import Path

spec=importlib.util.spec_from_file_location('resources',Path(__file__).parents[1]/'scripts/latency/summarize_resources.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)

class ResourcesTests(unittest.TestCase):
    def test_duplicate_fds_do_not_multiply_allocations(self):
        record='drm-pdev: 0000:65:00.0\ndrm-client-id: 7\ndrm-memory-vram: 1024 KiB\ndrm-memory-gtt: 256 KiB\n'
        second=record.replace('id: 7','id: 8')
        self.assertEqual(module.drm_totals([record,record,second]),{'vram':2048,'gtt':512,'cpu':0})

    def test_slopes_use_elapsed_time_and_report_decline(self):
        result=module.describe([(1200,2000),(1260,1900),(1320,1800)])
        self.assertEqual(result['slope_per_minute'],-100)
        self.assertEqual(result['maximum'],2000)
        self.assertIsNone(module.describe([]))

    def test_cpu_uses_actual_sample_interval(self):
        self.assertEqual(module.cpu_cores((10,100),(20,2100),100),2)
        self.assertIsNone(module.cpu_cores((20,100),(20,200),100))
        self.assertIsNone(module.cpu_cores((10,100),(20,50),100))

if __name__=='__main__':unittest.main()
