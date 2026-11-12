#!/usr/bin/env python3
"""Summarize original Blender evidence without mistaking RFB/pipeline counts for FPS."""
import argparse
from collections import Counter
import json
import pathlib
import re


def fields(line):
    return dict(re.findall(r'\b([A-Za-z_][A-Za-z_0-9]*)=([^\s]+)', line))


def number(values, name):
    try:
        return int(values[name], 10)
    except (KeyError, ValueError):
        return None


def analyze(text, display=None):
    draws = []
    pipelines = []
    fallbacks = Counter()
    mismatches = 0
    teardown = []
    budgets = []
    fallback_details = []
    for line in text.splitlines():
        data = fields(line)
        if line.startswith('MESA_METAL_GPU_PRESENT '):
            draws.append(data)
        elif 'Metal_pipeline program=' in line:
            pipelines.append(data)
        elif 'pixel_mismatch ' in line:
            mismatches += 1
        elif 'context_release ' in line:
            teardown.append(data)
        elif line.startswith('MESA_METAL ') and 'draw_budget_exhausted ' in line:
            budgets.append(data)
        elif line.startswith('MESA_METAL ') and '_fallback' in line:
            # Reason messages are rate-limited; these are not total fallback counts.
            reason = re.search(r'\b([A-Za-z_]+_fallback)\b', line)
            if reason:
                fallbacks[reason.group(1)] += 1
                if len(fallback_details) < 32:
                    fallback_details.append({'kind': reason.group(1), 'fields': data})
    attributed = [d for d in draws if (d.get('program_label') or d.get('label')) and d.get('program')]
    per_program = Counter(d.get('program_label') or d['label'] for d in attributed)
    totals = {}
    missing_context_id = False
    for d in draws:
        if not d.get('context_id'):
            missing_context_id = True
            continue
        key = d.get('pid', '?') + ':' + d['context_id']
        # Values are cumulative per context, not duration of this draw.
        previous = totals.setdefault(key, {'submissions': 0, 'gpu_ns': 0})
        previous['submissions'] = max(previous['submissions'], number(d, 'Metal_submissions') or 0)
        previous['gpu_ns'] = max(previous['gpu_ns'], number(d, 'gpu_ns') or 0)
    timing_available = any((number(d, 'gpu_ns') or 0) > 0 for d in draws)
    display = display or {}
    return {
        'scope': display.get('environment', 'Unspecified execution environment; inspect execution-identity.json'),
        'gpu_present_events': len(draws),
        'gpu_present_events_with_changed_pixels': sum((number(d, 'changed_pixels') or 0) > 0 for d in draws),
        'oracle_verified_events': sum(number(d, 'verified') == 1 for d in draws),
        'replacement_without_oracle_events': sum(number(d, 'verified') == 0 for d in draws),
        'logged_pixel_mismatch_events': mismatches,
        'observed_fallback_reasons_lower_bound': dict(fallbacks),
        'fallback_detail_samples': fallback_details,
        'logged_draw_budget_exhaustion_events': budgets,
        'reported_context_teardown_totals': teardown,
        'created_pipeline_labels': sorted({p['label'] for p in pipelines if p.get('label')}),
        'draws_with_shader_attribution': len(attributed),
        'gpu_events_by_program_label': dict(per_program),
        'gpu_events_by_shader_label': dict(per_program), # legacy key; labels describe GL programs
        'observed_draw_budgets': sorted({number(d, 'budget') for d in draws if number(d, 'budget') is not None}),
        'complete_context_ids_in_present_events': bool(draws) and not missing_context_id,
        'cumulative_gpu_by_context': totals,
        'gpu_timing_available': timing_available,
        'aggregate_gpu_ns': sum(v['gpu_ns'] for v in totals.values()) if draws and not missing_context_id and timing_available else None,
        'aggregate_submissions': sum(v['submissions'] for v in totals.values()) if draws and not missing_context_id else None,
        'first_ui_seconds': display.get('first_ui_seconds'),
        'original_blender_ui_observed': display.get('ui_text_observed'),
        'input_sequence_sent': display.get('input_sequence_sent'),
        'modified_scene_saved': display.get('model_saved_marker'),
        'rfb_delivered_updates': display.get('frames'),
        'render_fps': None,
        'solid_viewport_gpu_verified': None,
        'notes': [
            'Created pipelines do not prove executed geometry; program debug labels are not shader source or solid-viewport pixel proof.',
            'Fallback reason logs are rate-limited and are a lower bound, not a complete count.',
            'GPU times/submissions are cumulative per context; do not add every event value. Zero GPU time is treated as unavailable, not zero-cost rendering.',
            'RFB delivered updates are not application GPU render FPS or final display FPS.',
            'Solid viewport verification requires original scene pixels and attributed actual geometry draws; this report never infers it.'
        ]
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--session', type=pathlib.Path, required=True)
    p.add_argument('--output', type=pathlib.Path)
    a = p.parse_args()
    report = a.session / 'blender-display.json'
    display = json.loads(report.read_text()) if report.exists() else None
    result = analyze((a.session / 'application.log').read_text(errors='replace'), display)
    result['evidence_path'] = str(a.session.resolve())
    result['execution_identity'] = json.loads((a.session / 'execution-identity.json').read_text()) if (a.session / 'execution-identity.json').exists() else None
    output = json.dumps(result, indent=2) + '\n'
    if a.output:
        a.output.parent.mkdir(parents=True, exist_ok=True)
        a.output.write_text(output)
    else:
        print(output, end='')


if __name__ == '__main__':
    main()
