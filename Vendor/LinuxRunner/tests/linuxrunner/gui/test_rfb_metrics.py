#!/usr/bin/env python3
"""Real RFBClient opt-in timing against a bounded loopback protocol fixture."""
import argparse
import json
import pathlib
import socket
import struct
import subprocess
import tempfile
import threading

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--probe', required=True)
p.add_argument('--output', required=True)
a = p.parse_args()
probe = pathlib.Path(a.probe).resolve()
out = pathlib.Path(a.output).resolve()
out.parent.mkdir(parents=True, exist_ok=True)
errors = []
server = socket.socket()
server.bind(('127.0.0.1', 0))
server.listen(1)
server.settimeout(6)
port = server.getsockname()[1]

def exact(s, n):
    data = bytearray()
    while len(data) < n:
        part = s.recv(n-len(data))
        if not part: raise EOFError('Unexpected native client disconnect')
        data.extend(part)
    return bytes(data)

def serve():
    try:
        s, _ = server.accept()
        with s:
            s.settimeout(6)
            s.sendall(b'RFB 003.008\n'); exact(s, 12)
            s.sendall(b'\x01\x02'); assert exact(s, 1) == b'\x02'
            s.sendall(bytes(range(16))); exact(s, 16)
            s.sendall(bytes(4)); exact(s, 1)
            s.sendall(struct.pack('!HH', 32, 32)+bytes(16)+bytes(4))
            exact(s, 20)
            encodings = exact(s, 4)
            exact(s, struct.unpack('!H', encodings[2:])[0]*4)
            exact(s, 10)
            s.sendall(b'\x00\x00\x00\x01'+struct.pack('!HHHHi', 0, 0, 32, 32, 0)+bytes(32*32*4))
            inputs = 0
            while inputs < 3:
                kind = exact(s, 1)[0]
                if kind == 3: exact(s, 9)
                elif kind == 4: exact(s, 7); inputs += 1
                elif kind == 5: exact(s, 5); inputs += 1
                else: raise AssertionError(f'Unexpected client message {kind}')
            # Wait for the probe to stop so all metrics callbacks are delivered.
            while s.recv(32): pass
    except Exception as e:
        errors.append(repr(e))
    finally:
        server.close()

with tempfile.TemporaryDirectory() as tmp:
    password = pathlib.Path(tmp)/'password'
    password.write_text('Test2345'); password.chmod(0o600)
    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    result = subprocess.run([str(probe), str(port), str(password), str(out)], capture_output=True, text=True, timeout=8)
    thread.join(timeout=7)
    assert result.returncode == 0, result.stderr
    assert not thread.is_alive() and not errors, errors
report = json.loads(out.read_text())
assert report['delivered_updates'] == 1
assert report['transport_events_observed'] == 4
updates = [e for e in report['recent_transport_events'] if e['stage'] == 'rfb_decode_delivery']
inputs = [e for e in report['recent_transport_events'] if e['stage'] == 'rfb_input_write']
assert len(updates) == 1 and len(inputs) == 3
assert report['input_transport_events_observed'] == 3
assert report['recent_input_transport_events'] == inputs
assert updates[0]['rectangles'] == 1
assert updates[0]['damage_bytes'] == updates[0]['full_frame_bytes'] == 4096
assert updates[0]['decode_delivery_ms'] >= 0
assert sorted(e['packet_type'] for e in inputs) == [4, 4, 5]
assert sorted(e['packet_bytes'] for e in inputs) == [6, 8, 8]
assert all(e['written'] and e['input_queue_ms'] >= 0 and e['write_ms'] >= 0 for e in inputs)
assert [e['input_sequence'] for e in inputs] == [1, 2, 3]
assert all(e['write_begin_monotonic_ms'] >= e['queued_monotonic_ms'] > 0 for e in inputs)
assert all('keysym' not in e and 'pixels' not in e for e in inputs)
print('RFB_METRICS_PASS actual decoder/input stages, pointer/key writes, bounded metadata without input contents')
