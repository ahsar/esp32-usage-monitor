#!/usr/bin/env python3
"""Measure the chime's tone for dropouts, from a phone recording of the device.

    tools/chime_analyse.py recording.m4a            # loudest note in the file
    tools/chime_analyse.py recording.wav --t0 1.2 --t1 1.9
    tools/chime_analyse.py recording.m4a --channel L --freq 660

Why this exists: the chimes are streamed to the ES8311 through the I2S DMA, and a
writer that does not keep up with the DAC leaves holes that `auto_clear` fills
with zeros. Those holes are short (1-15 ms) and regular, so they do not read as
"silence" — they read as a fine rasp on top of the note, which is very hard to
argue about by ear ("还是有点爆音") and very easy to see: demodulate the note,
undo its (known, exponential) decay, and look at what is left.

A healthy chime's envelope is a clean exponential from the attack to the tail. A
starved one has periodic notches, and the spacing is the writer's cadence — 16 ms
for a loop()-driven writer (one 256-sample chunk per iteration), which is what
the "58 Hz rasp" was. Use the same recording before and after a change.

How to record: 30-50 cm from the speaker, in a quiet room. A phone held close to a
loud chime engages its own limiter, which *attenuates the first note of the take by
tens of dB* and compresses the rest of the decay — that is a property of the phone,
not of the device, and it is visible as the two stereo channels disagreeing about
the note levels.

No dependencies: stdlib only, plus macOS' afconvert to decode the phone's .m4a.
"""

import argparse
import cmath
import math
import os
import struct
import subprocess
import sys
import tempfile
import wave


# ---- loading ---------------------------------------------------------------

def to_wav(path):
    """afconvert the input to a 16-bit PCM WAV in the temp dir, if not WAV."""
    with open(path, "rb") as f:
        magic = f.read(4)
    if magic == b"RIFF":
        return path
    out = os.path.join(tempfile.gettempdir(),
                       os.path.basename(os.path.splitext(path)[0]) + ".chime.wav")
    if not os.path.exists(out):
        try:
            subprocess.run(["afconvert", "-f", "WAVE", "-d", "LEI16", path, out],
                           check=True, capture_output=True)
        except FileNotFoundError:
            sys.exit("afconvert not found: pass a WAV (ffmpeg -i in.m4a out.wav)")
        except subprocess.CalledProcessError as e:
            sys.exit("afconvert failed: %s" % e.stderr.decode(errors="replace"))
    return out


def load(path):
    w = wave.open(path)
    if w.getsampwidth() != 2:
        sys.exit("expected 16-bit PCM, got %d bytes/sample" % w.getsampwidth())
    n, ch, sr = w.getnframes(), w.getnchannels(), w.getframerate()
    s = struct.unpack("<%dh" % (n * ch), w.readframes(n))
    return [s[i::ch] for i in range(ch)], sr


# ---- small DSP -------------------------------------------------------------

def db(v):
    return 20 * math.log10(max(v, 1e-12))


def db_to_ln(x):
    return x * math.log(10) / 20


def rms_env(x, sr, win_ms=5.0):
    n = max(1, int(sr * win_ms / 1000))
    return [(i / sr, (sum(v * v for v in x[i:i + n]) / n) ** 0.5 / 32768)
            for i in range(0, len(x) - n, n)]


def fft(x):
    n = len(x)
    if n == 1:
        return x[:]
    ev, od = fft(x[0::2]), fft(x[1::2])
    out = [0j] * n
    for k in range(n // 2):
        t = cmath.exp(-2j * math.pi * k / n) * od[k]
        out[k] = ev[k] + t
        out[k + n // 2] = ev[k] - t
    return out


def peak_freq(x, sr, lo=150.0, hi=4000.0):
    """Dominant frequency of `x` (Hann-windowed, zero-padded to a power of 2)."""
    n = 1
    while n < len(x):
        n *= 2
    seg = [x[i] * (0.5 - 0.5 * math.cos(2 * math.pi * i / (len(x) - 1)))
           for i in range(len(x))] + [0.0] * (n - len(x))
    S = fft(seg)
    df = sr / n
    k0, k1 = max(1, int(lo / df)), min(n // 2 - 1, int(hi / df))
    k = max(range(k0, k1), key=lambda i: abs(S[i]))
    return k * df


def demod_env(x, f, sr, lp_ms=0.8, hop_ms=0.25):
    """|envelope| of the tone at `f` (complex demodulation + moving average)."""
    w = max(1, int(sr * lp_ms / 1000))
    hop = max(1, int(sr * hop_ms / 1000))
    k = 2 * math.pi * f / sr
    acc = sum(x[i] * cmath.exp(-1j * k * i) for i in range(w))
    out = []
    i = w
    while i + hop < len(x):
        out.append(((i + hop / 2) / sr, abs(acc) / w / 32768))
        for j in range(hop):
            acc += (x[i + j] * cmath.exp(-1j * k * (i + j))
                    - x[i + j - w] * cmath.exp(-1j * k * (i + j - w)))
        i += hop
    return out


def fit_tau(env, t_peak, t_end):
    """Decay constant of the note, least squares on log(env) with notches rejected.

    A notch is a dropout, not the decay, so the fit has to survive them: fit,
    throw away everything more than 4 dB below the fit, refit.
    """
    pts = [(t, v) for t, v in env if t_peak <= t <= t_end and v > 0]
    if len(pts) < 10:
        return None
    keep, tau = pts, None
    for _ in range(3):
        xs = [t for t, _ in keep]
        ys = [math.log(v) for _, v in keep]
        mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
        den = sum((x - mx) ** 2 for x in xs)
        slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den if den else 0.0
        if slope >= -1e-6:
            return None
        tau = -1.0 / slope
        fit = lambda t: my + slope * (t - mx)
        keep = [(t, v) for t, v in pts if math.log(v) > fit(t) - db_to_ln(4)]
    return tau


def median(xs):
    xs = sorted(xs)
    n = len(xs)
    return xs[n // 2] if n else float("nan")


def percentile(xs, p):
    xs = sorted(xs)
    if not xs:
        return float("nan")
    return xs[min(len(xs) - 1, int(p / 100.0 * len(xs)))]


def find_notches(env, t_peak, t_end, thresh_db=8.0, med_ms=10.0):
    """Groups of samples whose envelope falls `thresh_db` below a local median.

    The reference is a 10 ms median of the log-envelope, not the fitted
    exponential: the question is "does this tone have holes in it", and a phone's
    AGC changes the decay's *slope* (it compresses a loud recording a lot) without
    being able to invent 1-15 ms holes. A notch is measured against the level
    around it, which makes the test independent of the recording level.

    Returns (t, depth_dB, width_s) per event, where the width spans the samples
    below (median - 3 dB) — the hole, not just its bottom.
    """
    pts = [(t, v) for t, v in env if t_peak <= t <= t_end]
    if len(pts) < 8:
        return []
    hop = pts[1][0] - pts[0][0]
    lvl = [db(v) for _, v in pts]
    w = max(3, int(med_ms / 1000.0 / hop) | 1)
    med = [median(lvl[max(0, i - w // 2):i + w // 2 + 1]) for i in range(len(lvl))]
    # Spans below "the level around them" by 3 dB...
    spans, i = [], 1
    while i < len(lvl) - 1:
        if lvl[i] < med[i] - 3.0:
            j = i
            while j < len(lvl) and lvl[j] < med[j] - 3.0:
                j += 1
            spans.append((i, j))
            i = j
        i += 1
    # ...merged across gaps shorter than 5 ms: a hole wide enough to swallow the
    # median window (10 ms) comes back as several spans, and it is one dropout.
    merged = []
    for a, b in spans:
        if merged and pts[a][0] - pts[merged[-1][1] - 1][0] < 0.005:
            merged[-1] = (merged[-1][0], b)
        else:
            merged.append((a, b))
    out = []
    for a, b in merged:
        lo = min(range(a, b), key=lambda k: lvl[k] - med[k])
        if lvl[lo] - med[lo] <= -thresh_db:      # deep enough to be dropout, not noise
            out.append((pts[lo][0], lvl[lo] - med[lo], pts[b - 1][0] - pts[a][0]))
    return out


# ---- report ----------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recording")
    ap.add_argument("--channel", choices=("auto", "L", "R"), default="auto")
    ap.add_argument("--freq", type=float, help="tone to demodulate (default: loudest)")
    ap.add_argument("--t0", type=float, help="window start, s (default: the peak)")
    ap.add_argument("--t1", type=float, help="window end, s (default: peak + 0.3 s)")
    ap.add_argument("--thresh", type=float, default=8.0,
                    help="dropout threshold, dB below the level around it (default 8)")
    a = ap.parse_args()

    path = to_wav(a.recording)
    chans, sr = load(path)
    names = ["L", "R"] if len(chans) > 1 else ["L"]
    if a.channel == "auto":
        name, x = max(zip(names, chans), key=lambda p: sum(v * v for v in p[1]))
    else:
        name = "R" if a.channel == "R" and len(chans) > 1 else "L"
        x = chans[1 if name == "R" else 0]

    env = rms_env(x, sr, 5.0)
    t_peak, _ = max(env, key=lambda p: p[1])
    t0 = a.t0 if a.t0 is not None else max(0.0, t_peak - 0.02)
    t1 = a.t1 if a.t1 is not None else min(len(x) / sr, t0 + 0.30)
    seg = x[int(t0 * sr):int(t1 * sr)]
    if len(seg) < sr // 100:
        sys.exit("no signal in [%.3f, %.3f] s" % (t0, t1))

    f = a.freq or peak_freq(seg, sr)
    e = demod_env(x, f, sr)
    peak_t, peak_v = max(((t, v) for t, v in e if t0 <= t <= t1), key=lambda p: p[1])
    # The note is over once the envelope has fallen 25 dB below its peak and
    # stayed there: past that it is room tone, and every fluctuation in it would
    # otherwise be reported as a dropout.
    floor, dead = peak_v * 10 ** (-25 / 20), None
    for i, (t, v) in enumerate(e):
        if t < peak_t or v > floor:
            continue
        if all(w <= floor for _, w in e[i:i + 160]):   # ... and stayed there (40 ms)
            dead = t
            break
    t_end = min(t1, dead) if dead else t1
    tau = fit_tau(e, peak_t, t_end)
    notches = find_notches(e, peak_t, t_end, a.thresh)
    # The attack ramps in over ~8 ms; nothing there is a dropout.
    notches = [n for n in notches if n[0] - peak_t > 0.012]

    print("file     : %s (%d Hz, %d ch)" % (a.recording, sr, len(chans)))
    print("channel  : %s" % name)
    print("note     : %.1f Hz, t=%.3f..%.3f s, peak %.1f dBFS (decay tau %s)"
          % (f, peak_t, t_end, db(peak_v), "%.0f ms" % (tau * 1000) if tau else "?"))
    if not notches:
        print("dropouts : none above %.0f dB below the level around them" % a.thresh)
        print("verdict  : CLEAN — the note has no holes in it")
        return 0
    print("dropouts : %d (depth vs the level around them / width / spacing)" % len(notches))
    prev = None
    for t, d, w in notches:
        gap = "" if prev is None else "  +%5.2f ms" % ((t - prev) * 1000)
        print("           t=%.4f s  %6.1f dB  %5.2f ms%s" % (t, d, w * 1000, gap))
        prev = t
    gaps = [(notches[i + 1][0] - notches[i][0]) * 1000 for i in range(len(notches) - 1)]
    if not gaps:
        print("verdict  : one dropout, not periodic")
        return 1
    m = median(gaps)
    # A dropout can be wide enough to push the next one out, so the *cadence* is
    # the low end of the spacing distribution, not its middle.
    cadence = percentile(gaps, 25)
    print("           spacing: median %.2f ms, cadence (p25) %.2f ms"
          % (m, cadence))
    if not (6.0 <= cadence <= 45.0):
        print("verdict  : dropouts, but not at a writer-like cadence")
        return 1
    chunk = ("; one 256-sample chunk of a loop()-driven writer is 16 ms"
             if 13.0 <= cadence <= 20.0 else "")
    print("verdict  : PERIODIC DROPOUTS every ~%.1f ms — the stream is not keeping "
          "the I2S DMA fed%s" % (cadence, chunk))
    return 1


if __name__ == "__main__":
    sys.exit(main())
