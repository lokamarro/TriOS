import importlib.util
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('metrics', ROOT / 'scripts/analyze-blender-metal.py')
metrics = importlib.util.module_from_spec(spec)
spec.loader.exec_module(metrics)


class BlenderMetricsTests(unittest.TestCase):
    def test_old_logs_cannot_identify_solid_or_sum_contexts(self):
        text = '''MESA_METAL pid=56 Metal_pipeline program=9 label=SHD-workbench
MESA_METAL_GPU_PRESENT pid=56 draw=1 verified=1 changed_pixels=10 Metal_submissions=1 gpu_ns=100
MESA_METAL_GPU_PRESENT pid=56 draw=2 verified=1 changed_pixels=0 Metal_submissions=2 gpu_ns=200
MESA_METAL pid=56 texture_fallback internal=881a
'''
        r = metrics.analyze(text, {'frames': 99, 'first_ui_seconds': 30})
        self.assertEqual(r['gpu_present_events'], 2)
        self.assertEqual(r['gpu_present_events_with_changed_pixels'], 1)
        self.assertEqual(r['created_pipeline_labels'], ['SHD-workbench'])
        self.assertEqual(r['draws_with_shader_attribution'], 0)
        self.assertIsNone(r['aggregate_gpu_ns'])
        self.assertIsNone(r['solid_viewport_gpu_verified'])
        self.assertIsNone(r['render_fps'])
        self.assertEqual(r['observed_fallback_reasons_lower_bound'], {'texture_fallback': 1})

    def test_cumulative_timing_is_not_added_per_draw(self):
        text = '''MESA_METAL_GPU_PRESENT pid=56 context_id=A program=7 label=UI verified=1 changed_pixels=1 Metal_submissions=1 gpu_ns=100
MESA_METAL_GPU_PRESENT pid=56 context_id=B program=8 label=grid verified=0 changed_pixels=20 Metal_submissions=1 gpu_ns=40
MESA_METAL_GPU_PRESENT pid=56 context_id=A program=7 label=UI verified=1 changed_pixels=1 Metal_submissions=2 gpu_ns=150
MESA_METAL pid=56 pixel_mismatch program_draw=1 components=12 keeping=Mesa
'''
        r = metrics.analyze(text)
        self.assertEqual(r['aggregate_gpu_ns'], 190)
        self.assertEqual(r['aggregate_submissions'], 3)
        self.assertEqual(r['gpu_events_by_shader_label'], {'UI': 2, 'grid': 1})
        self.assertEqual(r['replacement_without_oracle_events'], 1)
        self.assertEqual(r['logged_pixel_mismatch_events'], 1)

    def test_current_program_labels_and_explicit_budget(self):
        text = "MESA_METAL_GPU_PRESENT pid=56 context_id=2 program=8 program_label=SHD-OCIO_Display verified=1 changed_pixels=4 Metal_submissions=3 gpu_ns=200 budget=128"
        r = metrics.analyze(text)
        self.assertEqual(r['gpu_events_by_program_label'], {'SHD-OCIO_Display': 1})
        self.assertEqual(r['draws_with_shader_attribution'], 1)
        self.assertEqual(r['observed_draw_budgets'], [128])
        self.assertIsNone(r['solid_viewport_gpu_verified'])

    def test_unavailable_gpu_clock_is_not_zero_cost_rendering(self):
        r = metrics.analyze('MESA_METAL_GPU_PRESENT pid=1 context_id=2 verified=1 Metal_submissions=2 gpu_ns=0')
        self.assertFalse(r['gpu_timing_available'])
        self.assertIsNone(r['aggregate_gpu_ns'])
        self.assertEqual(r['aggregate_submissions'], 2)

    def test_fallback_evidence_remains_bounded_and_budget_is_separate(self):
        text = '\n'.join('MESA_METAL pid=1 texture_fallback internal=8c43 width=512 height=512' for _ in range(40))
        text += '\nMESA_METAL pid=1 draw_budget_exhausted context_id=2 requested=128 committed=128 program=45'
        r = metrics.analyze(text)
        self.assertEqual(r['observed_fallback_reasons_lower_bound'], {'texture_fallback': 40})
        self.assertEqual(len(r['fallback_detail_samples']), 32)
        self.assertEqual(r['fallback_detail_samples'][0]['fields']['internal'], '8c43')
        self.assertEqual(r['logged_draw_budget_exhaustion_events'][0]['requested'], '128')
        self.assertEqual(r['gpu_present_events'], 0)
        self.assertIsNone(r['solid_viewport_gpu_verified'])


if __name__ == '__main__':
    unittest.main()
