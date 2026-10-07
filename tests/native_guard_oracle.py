"""Exact independent decode oracle for the opt-in native tone transitions."""
import array
import json
from fractions import Fraction
from math import ceil
from pathlib import Path
import subprocess


def validate_guard(ffmpeg, flv):
    def run(args):
        result = subprocess.run(args, capture_output=True, timeout=8)
        assert result.returncode == 0, result.stderr.decode(errors='replace')
        return result.stdout

    probe = str(Path(ffmpeg).with_name('ffprobe.exe'))
    records = json.loads(run([probe, '-v', 'error', '-show_frames', '-show_entries',
                              'frame=media_type,pts_time,nb_samples', '-of', 'json', str(flv)]))['frames']
    video = [f for f in records if f['media_type'] == 'video']
    audio = [f for f in records if f['media_type'] == 'audio']
    rgb = run([ffmpeg, '-v', 'error', '-xerror', '-i', str(flv), '-map', '0:v:0',
               '-vf', 'scale=1:1', '-fps_mode', 'passthrough', '-pix_fmt', 'rgb24', '-f', 'rawvideo', '-'])
    assert len(rgb) == len(video)*3
    display = []
    for i, frame in enumerate(video):
        r, g, b = rgb[i*3:i*3+3]
        blue = b > 150 and r < 80 and g < 80
        assert blue or (r > 150 and b < 80) or (g > 150 and b < 80), 'unexpected native picture'
        display.append((Fraction(frame['pts_time']), blue))
    assert rgb[0] > 150 and rgb[2] < 80, 'native guard test must also start with retained red programme'
    display.sort()
    runs = sum(blue and (i == 0 or not display[i-1][1]) for i, (_, blue) in enumerate(display))
    assert runs == 4, f'all four real native holding transitions required, got {runs}'
    intervals = [(t, display[i+1][0] if i+1 < len(display) else t+Fraction(1, 30))
                 for i, (t, blue) in enumerate(display) if blue]
    pcm = array.array('f')
    pcm.frombytes(run([ffmpeg, '-v', 'error', '-xerror', '-i', str(flv), '-map', '0:a:0',
                      '-c:a', 'pcm_f32le', '-f', 'f32le', '-']))
    assert len(pcm) == sum(int(f['nb_samples'])*2 for f in audio)
    checked = leaks = nonzero = index = 0
    for frame in audio:
        n = int(frame['nb_samples'])
        samples = pcm[index:index+n*2]
        index += n*2
        t = Fraction(frame['pts_time'])
        covered = set()
        for begin, end in intervals:
            first = min(n, max(0, ceil((begin-t)*48000)))
            last = min(n, max(first, ceil((end-t)*48000)))
            covered.update(range(first, last))
        for i in covered:
            for channel in range(2):
                checked += 1
                leaks += samples[i*2+channel] != 0
        nonzero += sum(value != 0 for value in samples)
    result = {'holding_runs': runs, 'overlapping_channel_samples': checked, 'nonzero_overlapping_samples': leaks,
              'nonzero_programme_samples': nonzero, 'decoded_frames': len(video)}
    Path(str(flv)+'.guard.json').write_text(json.dumps(result, indent=2))
    assert checked > 0 and nonzero > 0 and leaks == 0, result
    return result
