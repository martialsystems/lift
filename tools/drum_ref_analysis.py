#!/usr/bin/env python3
# Copyright (c) 2026 Martial Systems LLC. All rights reserved.
"""Compare LIFT's synthesized 808/909 voices with reference one-shots.

The reference samples are third-party recordings: they stay OUTSIDE the repo
(default /workspace/lift-design/drum-refs, or --refs DIR). Nothing derived from
them is written into the repo; renders of our own voices go to a temp dir.

  python3 tools/drum_ref_analysis.py --render build/lift_drum_render [--refs DIR]
  python3 tools/drum_ref_analysis.py ... --fit      # coordinate search on decay/tune/tone

Measures per sound: -20 / -40 dB decay (RMS envelope, 2 ms windows, from the
peak), attack (10 % -> peak), spectral centroid and band energies, pitch and
pitch envelope (kick, toms), and for claps the burst count, spacing and tail.
"""
import argparse, os, subprocess, sys, tempfile
import numpy as np
from scipy.io import wavfile
from scipy.signal import butter, sosfilt, find_peaks

REFS = {  # (kit, voice index, label) -> reference file under the refs dir
    (0, 0, 'BD'): 'TR/808/808BDRUM.WAV', (0, 2, 'SD'): 'TR/808/808SNARE.WAV', (0, 4, 'CP'): 'TR/808/808CLAP.WAV',
    (0, 8, 'CH'): 'TR/808/808CLHAT.WAV', (0, 9, 'OH'): 'TR/808/808OPHAT.WAV',
    (1, 1, 'BD'): 'TR/909/909BDRUM.WAV', (1, 2, 'SD'): 'TR/909/909SNARE.WAV', (1, 4, 'CP'): 'TR/909/909CLAP.WAV',
    (1, 8, 'CH'): 'TR/909/909CLHAT.WAV', (1, 9, 'OH'): 'TR/909/909OPHAT.WAV',
    (1, 11, 'LT'): 'TR/909/909LTOM.WAV', (1, 12, 'MT'): 'TR/909/909MTOM.WAV', (1, 13, 'HT'): 'TR/909/909HTOM.WAV',
}


def load(path):
    sr, x = wavfile.read(path)
    x = x.astype(np.float64)
    if x.ndim > 1:
        x = x.mean(axis=1)
    if np.issubdtype(np.dtype(type(x.flat[0])), np.floating) and np.abs(x).max() > 2:
        x /= 32768.0
    m = np.abs(x).max()
    return sr, (x / m if m > 0 else x)


def env_db(x, sr, win=0.002):
    n = max(1, int(win * sr))
    k = len(x) // n
    e = np.sqrt(np.mean(x[:k * n].reshape(k, n) ** 2, axis=1) + 1e-20)
    return 20 * np.log10(e / e.max()), n / sr


def decay(x, sr, db):
    e, dt = env_db(x, sr)
    p = int(np.argmax(e))
    below = np.where(e[p:] > db)[0]
    last = p + (below[-1] if len(below) else 0)
    t = (last + 1 - p) * dt
    truncated = last + 1 >= len(e) - 1
    return 1000 * t, truncated


def attack(x, sr):
    a = np.abs(x)
    p = int(np.argmax(a))
    s = np.where(a[:p + 1] >= 0.1)[0]
    return 1000 * (p - (s[0] if len(s) else 0)) / sr


def spectrum(x, sr, upto_ms=None):
    if upto_ms:
        x = x[:int(upto_ms * sr / 1000)]
    X = np.abs(np.fft.rfft(x * np.hanning(len(x)))) ** 2
    f = np.fft.rfftfreq(len(x), 1 / sr)
    tot = X.sum() + 1e-30
    cen = float((f * X).sum() / tot)
    bands = [(0, 200), (200, 2000), (2000, 8000), (8000, sr / 2)]
    be = [float(X[(f >= a) & (f < b)].sum() / tot) for a, b in bands]
    return cen, be


def pitch_track(x, sr, lo=30, hi=600):
    out = []
    for t in (0.005, 0.03, 0.1, 0.25):
        i, n = int(t * sr), int(0.04 * sr)
        seg = x[i:i + n]
        if len(seg) < n // 2:
            out.append(float('nan'))
            continue
        N = 1 << 15
        X = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), N))
        f = np.fft.rfftfreq(N, 1 / sr)
        m = (f >= lo) & (f <= hi)
        out.append(float(f[m][np.argmax(X[m])]))
    return out


def clap_shape(x, sr):
    sos = butter(4, 600, 'hp', fs=sr, output='sos')
    y = np.abs(sosfilt(sos, x))
    n = max(1, int(0.0005 * sr))
    k = len(y) // n
    e = y[:k * n].reshape(k, n).max(axis=1)
    sm = np.convolve(e, np.ones(3) / 3, 'same')
    first = sm[:int(0.06 * sr / n)]
    pk, _ = find_peaks(first, prominence=0.12 * first.max(), distance=max(1, int(0.004 * sr / n)))
    times = pk * n / sr * 1000
    spacing = float(np.median(np.diff(times))) if len(times) > 1 else float('nan')
    tail_from = int(times[-1] / 1000 * sr) if len(times) else 0
    tail, _ = decay(x[tail_from:], sr, -20)
    return len(times), spacing, tail


def analyse(path, label):
    sr, x = load(path)
    on = np.where(np.abs(x) > 0.02)[0]
    x = x[on[0]:] if len(on) else x
    d20, tr20 = decay(x, sr, -20)
    d40, tr40 = decay(x, sr, -40)
    cen, be = spectrum(x, sr, upto_ms=max(30.0, d20))
    r = dict(d20=d20, d40=d40, tr40=tr40, att=attack(x, sr), cen=cen, bands=be)
    if label in ('BD', 'LT', 'MT', 'HT'):
        r['pitch'] = pitch_track(x, sr, 30 if label == 'BD' else 60, 300 if label == 'BD' else 600)
    if label == 'CP':
        r['clap'] = clap_shape(x, sr)
    return r


def render(tool, kit, voice, overrides, tmp):
    out = os.path.join(tmp, f'k{kit}v{voice}.wav')
    subprocess.run([tool, str(kit), str(voice), out] + [f'{k}={v}' for k, v in overrides.items()], check=True)
    return out


def cost(ref, ours):
    c = (np.log(ours['d20'] / ref['d20'])) ** 2 * 4
    c += (np.log(ours['cen'] / ref['cen'])) ** 2 * 2
    c += sum((a - b) ** 2 for a, b in zip(ref['bands'], ours['bands']))
    if 'pitch' in ref:
        for a, b in zip(ref['pitch'][:3], ours['pitch'][:3]):
            if a == a and b == b:
                c += (np.log(b / a)) ** 2 * 3
    return float(c)


BEND0 = {(0, 'BD'): 3.0, (1, 'BD'): 20.0, (1, 'LT'): 4.0, (1, 'MT'): 4.0, (1, 'HT'): 4.0}

FIT = {  # (kit, label) -> SHOGUN params searched (decay, pitch, tone); LIFT:BEND = hit pitch drop, semitones
    (0, 'BD'): ['BD1:DECAY', 'BD1:TUNE', 'BD1:PITCH', 'LIFT:BEND'], (1, 'BD'): ['BD2:DECAY', 'BD2:TUNE', 'BD2:TONE', 'LIFT:BEND'],
    (0, 'SD'): ['SD:SN.DEC', 'SD:T.DECAY', 'SD:TONE', 'SD:TUNE', 'SD:SNAPPY'],
    (1, 'SD'): ['SD:SN.DEC', 'SD:T.DECAY', 'SD:TONE', 'SD:TUNE', 'SD:SNAPPY'],
    (0, 'CP'): ['CP:DECAY', 'CP:FILTER', 'CP:SOUND'], (1, 'CP'): ['CP:DECAY', 'CP:FILTER', 'CP:SOUND'],
    (0, 'CH'): ['CH:DECAY', 'CH:TUNE'], (1, 'CH'): ['CH:DECAY', 'CH:TUNE'],
    (0, 'OH'): ['OH:DECAY'], (1, 'OH'): ['OH:DECAY'],
    (1, 'LT'): ['LTC:DECAY', 'LTC:TUNE', 'LIFT:BEND'], (1, 'MT'): ['MTC:DECAY', 'MTC:TUNE', 'LIFT:BEND'],
    (1, 'HT'): ['HTC:DECAY', 'HTC:TUNE', 'LIFT:BEND'],
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--refs', default='/workspace/lift-design/drum-refs')
    ap.add_argument('--render', required=True, help='path to the lift_drum_render binary')
    ap.add_argument('--fit', action='store_true')
    ap.add_argument('--start', default='', help='python dict of current kit values per (kit,label), for --fit')
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix='lift-drumfit-')
    start = eval(a.start) if a.start else {}
    print(f"{'voice':8} {'ref d20':>8} {'ours':>6} {'ref d40':>8} {'ours':>6} {'ref cen':>8} {'ours':>6}  extra")
    fitted = {}
    for (kit, v, lab), rel in REFS.items():
        ref = analyse(os.path.join(a.refs, rel), lab)
        ov = dict(start.get((kit, lab), {}))
        if a.fit and (kit, lab) in FIT:
            params = FIT[(kit, lab)]
            got = subprocess.run([a.render, str(kit), str(v), '--get'] + [q for q in params if not q.startswith('LIFT:')], check=True, capture_output=True, text=True).stdout
            for line in got.split():
                k, val = line.split('=')
                ov.setdefault(k, float(val))
            if 'LIFT:BEND' in params:
                ov.setdefault('LIFT:BEND', BEND0.get((kit, lab), 0.0))
            best = cost(ref, analyse(render(a.render, kit, v, ov, tmp), lab))
            for rnd, step in enumerate((0.08, 0.04, 0.02, 0.01)):
                for p in params:
                    for sgn in (1, -1):
                        while True:
                            cur = ov.get(p)
                            if cur is None:
                                break
                            trial = dict(ov)
                            trial[p] = (max(0.0, cur + sgn * step * 25) if p == 'LIFT:BEND'
                                        else min(1.0, max(0.0, cur + sgn * step)))
                            c = cost(ref, analyse(render(a.render, kit, v, trial, tmp), lab))
                            if c < best - 1e-6:
                                best, ov = c, trial
                            else:
                                break
            fitted[(kit, lab)] = ov
        ours = analyse(render(a.render, kit, v, ov, tmp), lab)
        name = f"{'808' if kit == 0 else '909'} {lab}"
        extra = ''
        if 'pitch' in ref:
            extra = 'pitch ref ' + '/'.join(f'{p:.0f}' for p in ref['pitch'][:3]) + ' ours ' + '/'.join(f'{p:.0f}' for p in ours['pitch'][:3])
        if 'clap' in ref:
            rc, oc = ref['clap'], ours['clap']
            extra = f'bursts ref {rc[0]}x{rc[1]:.1f}ms tail {rc[2]:.0f}ms, ours {oc[0]}x{oc[1]:.1f}ms tail {oc[2]:.0f}ms'
        t40 = '>' if ref['tr40'] else ' '
        print(f"{name:8} {ref['d20']:8.0f} {ours['d20']:6.0f} {t40}{ref['d40']:7.0f} {ours['d40']:6.0f} {ref['cen']:8.0f} {ours['cen']:6.0f}  {extra}")
    if fitted:
        print('FITTED', repr(fitted))


if __name__ == '__main__':
    main()
