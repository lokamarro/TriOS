#!/usr/bin/env python3
"""Classify available evidence conservatively; this is never an app acceptance test."""
import argparse,json,pathlib,re

def write_report(directory):
    directory=pathlib.Path(directory)
    def read(name):
        p=directory/name
        return p.read_text(errors='replace') if p.exists() else ''
    def data(name):
        try:return json.loads(read(name))
        except (ValueError,TypeError):return {}
    state=dict(re.findall(r'^(\w+)=(.*)$',read('window-state'),re.M))
    logs=read('launch.log')+'\n'+read('application.log')
    faults=[line for line in logs.splitlines() if 'GUEST_FAULT:' in line or ':FATAL:' in line][:32]
    snapshots=read('runtime-snapshots.log')
    transport=data('transport.json')
    acceptance=data('application-acceptance.json')
    pixels=[line for line in snapshots.splitlines() if line.startswith('X11_WINDOW_PIXELS')][-64:]
    frames=acceptance.get('frames',transport.get('frames',0))
    mapped=int(state.get('mapped','0'))
    storage_full=('database or disk is full' in logs or 'No space left on device' in logs)
    if storage_full:stage='FIXTURE_STORAGE_EXHAUSTED_RETEST_REQUIRED'
    elif acceptance.get('actual_content_visible'):stage='APPLICATION_CONTENT_DELIVERED'
    elif faults:stage='APPLICATION_FAILURE_REQUIRES_FIRST_FAULT_REVIEW'
    elif not frames:stage='NO_CAPTURED_RFB_FRAME'
    elif not mapped:stage='NO_APPLICATION_WINDOW_MAPPED'
    elif pixels:stage='WINDOW_MAPPED_COMPARE_X11_PIXELS_WITH_RFB'
    else:stage='WINDOW_MAPPED_APPLICATION_PAINT_UNVERIFIED'
    result={
        'diagnostic_only_not_application_pass':True,
        'environment':'macOS guest fixture; physical iOS presentation unverified',
        'furthest_evidenced_stage':stage,'mapped_application_windows':mapped,
        'rfb_frames':frames,'fatal_evidence':faults,
        'processes_observed':sorted(set(re.findall(r'^PROCESS pid=\d+ exe=(.+)$',snapshots,re.M))),
        'role_hints':sorted(set(re.findall(r'^ROLE (.+)$',snapshots,re.M))),
        'role_caveat':'Forked zygote cmdline may be stale; role hints are not authoritative.',
        'x11_pixel_samples':pixels,
        'xdamage':'Extension query only; damage-event timing is not measured.',
        'application_acceptance':acceptance,
        'guest_gpu_acceleration':'NOT established by mapping, pixel capture or RFB transport',
        'native_metal_presentation':'NOT exercised by this Mac RFB probe',
    }
    (directory/'black-screen-diagnostic.json').write_text(json.dumps(result,indent=2)+'\n')
    return result
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('directory');a=p.parse_args()
    print(json.dumps(write_report(a.directory),indent=2))
