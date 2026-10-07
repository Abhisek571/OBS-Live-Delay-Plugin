"""Isolated real libobs -> compressed prebuffer -> local RTMP -> decode.
No OBS application, profiles, real services, keys, or persistent listener.
"""
import json
import pathlib
import re
import socket
import subprocess
import sys

ffmpeg, harness, obs_root, artifact_dir, mode = sys.argv[1:]
artifacts = pathlib.Path(artifact_dir)
artifacts.mkdir(parents=True, exist_ok=True)
guard = mode.startswith('guard-')
if guard:
    mode = mode.removeprefix('guard-')
assert mode in ('single', 'multi')
ports = []
for _ in range(2 if mode == 'multi' else 1):
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        ports.append(reservation.getsockname()[1])
receivers = []
logs = []
files = []

def redact(text):
    return re.sub(r'rtmp://[^\s\]\"\']+', '<synthetic-loopback>', text)

def validate(flv):
    if guard:
        from native_guard_oracle import validate_guard
        return validate_guard(ffmpeg, flv)
    # Decode EVERY frame, including startup: red is retained programme,
    # green is current programme and blue is holding. No frames excluded.
    decoded = subprocess.run([ffmpeg, '-v', 'error', '-i', str(flv), '-map', '0:v:0',
                              '-vf', 'scale=1:1', '-pix_fmt', 'rgb24', '-f', 'rawvideo', '-'],
                             capture_output=True, timeout=5)
    if decoded.returncode:
        raise AssertionError(decoded.stderr.decode(errors='replace'))
    frames = [tuple(decoded.stdout[i:i+3]) for i in range(0, len(decoded.stdout), 3)]
    def label(rgb):
        r, g, b = rgb
        if r > 180 and g < 80 and b < 80:
            return 'programme-red'
        if g > 180 and r < 80 and b < 80:
            return 'programme-green'
        return 'unexpected'
    labels = list(map(label, frames))
    assert len(frames) >= 60, 'local receiver did not decode sufficient programme video'
    assert 'unexpected' not in labels, 'holding/black/corrupt frame was exposed'
    # The harness turns programme green just before Start, and the broadcast is live.
    green = labels.index('programme-green')
    assert green <= 5, f'broadcast did not start live: {green} pre-start frames'
    assert all(x == 'programme-green' for x in labels[green:]), 'programme time moved backwards'
    audio = subprocess.run([ffmpeg, '-v', 'error', '-i', str(flv), '-map', '0:a:0', '-f', 'f32le', '-'],
                           capture_output=True, timeout=5)
    assert audio.returncode == 0 and len(audio.stdout) >= 48000 * 2 * 4, 'AAC programme stream must decode'
    return {'decoded_frames': len(frames), 'first_rgb': frames[0], 'first_green_frame': green,
            'decoded_audio_bytes': len(audio.stdout), 'no_holding_and_live_start': True}

try:
    for index, port in enumerate(ports):
        url = f'rtmp://127.0.0.1:{port}/local/synthetic'
        flv = artifacts / f'prebuffer-loopback-{index}.flv'
        log = (artifacts / f'receiver-{index}.log').open('w', encoding='utf-8')
        logs.append(log)
        files.append(flv)
        receivers.append(subprocess.Popen([ffmpeg, '-hide_banner', '-loglevel', 'warning', '-listen', '1',
                                           '-i', url, '-map', '0', '-c', 'copy', '-y', str(flv)],
                                          stdout=log, stderr=subprocess.STDOUT))
    result = subprocess.run([harness, obs_root, str(ports[0]), str(ports[1] if mode == 'multi' else 0)] + (['--guard'] if guard else []),
                            capture_output=True, text=True, timeout=28 if guard else 20)
    (artifacts / 'native-capture.log').write_text(redact(result.stdout + result.stderr), encoding='utf-8')
    if result.returncode:
        raise AssertionError(f'native harness exit {result.returncode}: {redact(result.stderr[-1500:])}')
    for receiver in receivers:
        receiver.wait(timeout=3)
    report = {'mode': mode, 'harness_exit': result.returncode,
              'receiver_exits': [r.returncode for r in receivers], 'destinations': [validate(f) for f in files]}
    assert len(report['destinations']) == len(ports)
    (artifacts / 'decode.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report))
finally:
    for receiver in receivers:
        if receiver.poll() is None:
            receiver.terminate()
    for receiver in receivers:
        try:
            receiver.wait(timeout=2)
        except subprocess.TimeoutExpired:
            receiver.kill()
            receiver.wait(timeout=2)
    for log in logs:
        log.close()
        path = pathlib.Path(log.name)
        path.write_text(redact(path.read_text(encoding='utf-8')), encoding='utf-8')

