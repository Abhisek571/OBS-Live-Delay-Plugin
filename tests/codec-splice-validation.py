"""Existing FFmpeg only: encode fixtures, exercise production splice, decode independently."""
import array
from fractions import Fraction
import json
from math import ceil
from pathlib import Path
import subprocess
import sys

ffmpeg, harness, directory = sys.argv[1:4]
require_boundary_silence = "--require-boundary-silence" in sys.argv[4:]
def option(name, default):
    return next((int(arg.split('=', 1)[1]) for arg in sys.argv[4:] if arg.startswith(name+'=')), default)
rate = option('--rate', 48000)
channels = option('--channels', 2)
programme_bframes = option('--bframes', 2)
assert rate in (44100, 48000) and channels in (1, 2) and programme_bframes in (0, 2)
out = Path(directory)
out.mkdir(parents=True, exist_ok=True)

def run(args):
    result = subprocess.run(args, capture_output=True, text=True, timeout=20)
    print('COMMAND', ' '.join(map(str, args)), 'EXIT', result.returncode)
    if result.stdout:
        print(result.stdout if len(result.stdout) <= 2000 else f'[captured {len(result.stdout)} stdout characters]')
    if result.stderr:
        print(result.stderr)
    if result.returncode:
        raise RuntimeError('codec validation command failed')
    return result.stdout

for name, color, audio, duration, profile, bframes in [
    ('programme', 'red', f'sine=frequency=440:sample_rate={rate}', '10', 'high', str(programme_bframes)),
    ('holding', 'blue', f'anullsrc=r={rate}:cl={"stereo" if channels == 2 else "mono"}', '1', 'baseline', '0'),
]:
    run([ffmpeg, '-hide_banner', '-v', 'error', '-y', '-f', 'lavfi', '-i',
         f'color=c={color}:s=64x64:r=30', '-f', 'lavfi', '-i', audio,
         '-t', duration, '-c:v', 'libx264', '-profile:v', profile, '-pix_fmt', 'yuv420p',
         '-g', '30', '-keyint_min', '30', '-sc_threshold', '0', '-bf', bframes,
         '-c:a', 'aac', '-ar', str(rate), '-ac', str(channels), '-f', 'flv', str(out / f'{name}.flv')])
run([harness, str(out/'programme.flv'), str(out/'holding.flv'), str(out/'splice.flv')])
run([ffmpeg, '-hide_banner', '-v', 'error', '-xerror', '-y', '-i', str(out/'splice.flv'),
     '-map', '0:v:0', '-fps_mode', 'passthrough', '-pix_fmt', 'rgb24', '-f', 'rawvideo', str(out/'video.rgb')])
run([ffmpeg, '-hide_banner', '-v', 'error', '-xerror', '-y', '-i', str(out/'splice.flv'),
     '-map', '0:a:0', '-c:a', 'pcm_f32le', '-f', 'f32le', str(out/'audio.f32')])
# ffprobe ships with the same existing decoder distribution. No installation.
probe = str(Path(ffmpeg).with_name('ffprobe.exe' if Path(ffmpeg).suffix == '.exe' else 'ffprobe'))
frames = json.loads(run([probe, '-v', 'error', '-show_frames', '-show_entries',
                        'frame=media_type,pts_time,nb_samples', '-of', 'json', str(out/'splice.flv')]))['frames']
video = [f for f in frames if f['media_type'] == 'video']
audio = [f for f in frames if f['media_type'] == 'audio']
rgb = (out/'video.rgb').read_bytes()
assert len(rgb) == len(video)*64*64*3, 'decoded video/frame records agree'
blue = []
red = []
for i, frame in enumerate(video):
    r, g, b = rgb[i*64*64*3:i*64*64*3+3]
    t = float(frame['pts_time'])
    if b > 150 and r < 80:
        blue.append(t)
    elif r > 150 and b < 80:
        red.append(t)
    else:
        raise AssertionError(f'unexpected decoded colour {r,g,b}')
assert blue and red, 'both real H264 feeds decoded'
# Retain the whole-frame boundary check, and additionally check EVERY sample
# whose presentation time overlaps a displayed blue frame. No priming exclusion.
runs = []
for t in blue:
    if not runs or t-runs[-1][-1] > 0.1:
        runs.append([t])
    else:
        runs[-1].append(t)
assert len(runs) >= 4, 'all four independent holding splices decoded'
display = sorted((Fraction(frame['pts_time']), float(frame['pts_time']) in blue) for frame in video)
holding_intervals = [(t, display[i+1][0] if i+1 < len(display) else t+Fraction(1, 30))
                     for i, (t, is_blue) in enumerate(display) if is_blue]

def overlap_bounds(frame_pts, frame_samples, begin, end):
    # Half-open presentation intervals, exact rational arithmetic: no epsilon.
    first = min(frame_samples, max(0, ceil((begin-frame_pts)*rate)))
    last = min(frame_samples, ceil((end-frame_pts)*rate))
    return first, max(first, last)

# Guard inclusion of a partially overlapping AAC frame and exact endpoints.
assert overlap_bounds(Fraction(0), 1024, Fraction(1, rate), Fraction(3, rate)) == (1, 3)
assert overlap_bounds(Fraction(0), 1024, Fraction(-1), Fraction(1, rate)) == (0, 1)
assert overlap_bounds(Fraction(0), 1024, Fraction(1023, rate), Fraction(1)) == (1023, 1024)
pcm = array.array('f')
pcm.frombytes((out/'audio.f32').read_bytes())
assert sum(int(f['nb_samples'])*channels for f in audio) == len(pcm), 'decoded AAC frame sample accounting'
index = 0
zero_frames = 0
nonzero = 0
boundary_frames = []
overlap_frames = []
overlap_samples = 0
nonzero_overlap_samples = 0
for frame in audio:
    count = int(frame['nb_samples'])*channels
    samples = pcm[index:index+count]
    index += count
    t = float(frame['pts_time'])
    covered = set()
    for begin, end in holding_intervals:
        first, last = overlap_bounds(Fraction(frame['pts_time']), count//channels, begin, end)
        covered.update(range(first, last))
    overlapping = [samples[i*channels+c] for i in sorted(covered) for c in range(channels)]
    if overlapping:
        peak = max(map(abs, overlapping))
        overlap_samples += len(overlapping)
        nonzero_overlap_samples += sum(value != 0 for value in overlapping)
        if peak != 0:
            overlap_frames.append({'pts': t, 'peak': peak, 'covered_channel_samples': len(overlapping)})
    if any(t >= run[0] and t+count/(channels*rate) <= run[-1] for run in runs):
        boundary_frames.append({'pts': t, 'peak': max(map(abs, samples), default=0)})
    if any(t > run[0]+0.15 and t+count/(channels*rate) < run[-1]-0.15 for run in runs):
        assert max(map(abs, samples), default=0) == 0, 'holding AAC decodes to zero in holding interior'
        zero_frames += 1
    nonzero += sum(value != 0 for value in samples)
assert zero_frames > 0 and nonzero > 0, 'holding silence and identifiable original programme audio decoded'
result = {'decoded_video_frames': len(video), 'holding_runs': len(runs),
          'zero_holding_aac_frames': zero_frames, 'decoded_float_samples': len(pcm),
          'nonzero_programme_samples': nonzero,
          'holding_aac_frames_including_boundary': len(boundary_frames),
          'nonzero_holding_boundary_frames': sum(f['peak'] != 0 for f in boundary_frames),
          'holding_display_intervals': [[float(a), float(b)] for a, b in holding_intervals],
          'holding_overlap_channel_samples': overlap_samples,
          'nonzero_holding_overlap_channel_samples': nonzero_overlap_samples,
          'nonzero_holding_overlap_frames': overlap_frames,
          'strict_boundary_silence_passed': bool(boundary_frames) and overlap_samples > 0 and
              all(f['peak'] == 0 for f in boundary_frames) and nonzero_overlap_samples == 0,
          'nonzero_holding_boundaries': [f for f in boundary_frames if f['peak'] != 0]}
(out/'decode-evidence.json').write_text(json.dumps(result, indent=2))
print(json.dumps({k: v for k, v in result.items() if k != 'holding_display_intervals'}, indent=2))

if require_boundary_silence:
    assert result['strict_boundary_silence_passed'], 'Strict holding silence fails: nonzero first-splice PCM or samples overlapping displayed holding'
