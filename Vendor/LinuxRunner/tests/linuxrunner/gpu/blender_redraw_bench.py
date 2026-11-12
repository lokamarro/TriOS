# Blender redraw benchmark: once the UI loop runs, time real redraws with
# Blender's own wm.redraw_timer (3D viewport region, then whole window + swap),
# write the results and quit. Run: blender --factory-startup --python <this>.
# Output: $LR_BLENDER_BENCH_OUT (default /tmp/blender-bench.txt).
import bpy, ctypes, os, time

# Mesa-Metal bridge counters when the interposer is preloaded (reset per read).
try:
    _report = ctypes.CDLL(None).lrMesaMetalReport
    _report.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
except (OSError, AttributeError):
    _report = None


def bridge_report():
    if not _report:
        return ''
    buf = ctypes.create_string_buffer(4096)
    _report(buf, len(buf))
    return buf.value.decode(errors='replace').strip()

OUT = os.environ.get('LR_BLENDER_BENCH_OUT', '/tmp/blender-bench.txt')
ITERATIONS = int(os.environ.get('LR_BLENDER_BENCH_ITERATIONS', '10'))
started = time.perf_counter()


def run():
    lines = ['ui_ready_seconds=%.2f' % (time.perf_counter() - started)]
    try:
        window = bpy.context.window_manager.windows[0]
        area = next(a for a in window.screen.areas if a.type == 'VIEW_3D')
        region = next(r for r in area.regions if r.type == 'WINDOW')
        lines.append('viewport=%dx%d window=%dx%d' % (region.width, region.height, window.width, window.height))
        for shading in ('SOLID', 'WIREFRAME'):
            area.spaces.active.shading.type = shading
            with bpy.context.temp_override(window=window, area=area, region=region):
                for kind in ('DRAW', 'DRAW_WIN_SWAP'):
                    bpy.ops.wm.redraw_timer(type=kind, iterations=2)          # warm caches/shaders
                    bridge_report()
                    t = time.perf_counter()
                    bpy.ops.wm.redraw_timer(type=kind, iterations=ITERATIONS)
                    lines.append('%s %s ms_per_redraw=%.1f' % (shading, kind, (time.perf_counter() - t) * 1000 / ITERATIONS))
                    report = bridge_report()
                    if report:
                        lines.append('  bridge(%d redraws): %s' % (ITERATIONS, report))
        lines.append('BLENDER_BENCH_DONE')
    except Exception as e:  # report, do not hide
        lines.append('BLENDER_BENCH_ERROR %r' % (e,))
    with open(OUT, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    bpy.ops.wm.quit_blender()
    return None


bpy.app.timers.register(run, first_interval=1.0)
