import argparse
import bisect
from collections import Counter
from dataclasses import dataclass

import mido
import numpy as np

AARDEN_MAJ = np.array([17.7661, 0.145624, 14.9265, 0.160186, 19.8049, 11.3587,
                       0.291248, 22.062, 0.145624, 8.15494, 0.232998, 4.95122])
AARDEN_MIN = np.array([18.2648, 0.737619, 14.0499, 16.8599, 0.702494, 14.4362,
                       0.702494, 18.6161, 4.56621, 1.93186, 7.37619, 1.75623])

from mido.midifiles import meta as _meta

_ks_decode = _meta.MetaSpec_key_signature.decode


def _ks_decode_lenient(self, message, data):
    try:
        _ks_decode(self, message, data)
    except _meta.KeySignatureError:
        message.key = "C"


_meta.MetaSpec_key_signature.decode = _ks_decode_lenient

PC = ["C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"]
NC_DEGREE = 15
SOLO_DEGREE = 14
SOLO = "solo"
SOLO_ACC_MIN16 = 0.5
SOLO_MIN_ACC_NOTES = 0.10

TEMPLATES = [
    ("",     (0, 4, 7)),
    ("m",    (0, 3, 7)),
    ("7",    (0, 4, 7, 10)),
    ("m7",   (0, 3, 7, 10)),
    ("maj7", (0, 4, 7, 11)),
    ("5",    (0, 7)),
    ("sus4", (0, 5, 7)),
    ("sus2", (0, 2, 7)),
    ("dim",  (0, 3, 6)),
    ("aug",  (0, 4, 8)),
    ("m7b5", (0, 3, 6, 10)),
    ("dim7", (0, 3, 6, 9)),
    ("6",    (0, 4, 7, 9)),
    ("m6",   (0, 3, 7, 9)),
    ("add9", (0, 2, 4, 7)),
    ("madd9", (0, 2, 3, 7)),
]
MISS_PENALTY = 0.12
EXTRA_TONE_PENALTY = 0.06
BASS_ROOT_BONUS = 0.08
MELODY_PITCH, MELODY_WEIGHT = 72, 0.6
MIN_SEGMENT_WEIGHT = 1e-6


@dataclass
class Note:
    start: float
    end: float
    pitch: int
    voice: tuple = ()
    melody: bool = False


@dataclass
class Token:
    tonic: int
    mode: int
    degree: int
    mask: int
    bass: int
    bar: int
    onset: int
    dur: int
    barlen: int = 16
    cont: int = 0
    label: str = ""
    start16: float = -1.0

    @property
    def word_a(self):
        return (self.tonic | self.mode << 4 | self.degree << 5 | self.mask << 9
                | self.bass << 20 | (self.barlen - 1) << 24)

    @property
    def word_b(self):
        return self.bar | self.onset << 16 | self.dur << 22 | self.cont << 30

    @property
    def packed(self):
        return self.word_a | self.word_b << 32

    @property
    def is_nc(self):
        return self.degree >= SOLO_DEGREE

    @property
    def is_silence(self):
        return self.degree == NC_DEGREE

    @property
    def is_solo(self):
        return self.degree == SOLO_DEGREE

    @property
    def root_pc(self):
        return None if self.is_nc else (self.tonic + self.degree) % 12


def unpack(v):
    v = int(v)
    a, b = v & 0xFFFFFFFF, v >> 32
    return Token(tonic=a & 15, mode=a >> 4 & 1, degree=a >> 5 & 15, mask=a >> 9 & 0x7FF,
                 bass=a >> 20 & 15, barlen=(a >> 24 & 63) + 1, bar=b & 0xFFFF,
                 onset=b >> 16 & 63, dur=b >> 22 & 255, cont=b >> 30 & 1)


def unpack_stream(values):
    return [unpack(v) for v in values]


try:
    import symusic
except ImportError:
    symusic = None


def load(path):
    if symusic is not None:
        try:
            return _load_symusic(path)
        except Exception:
            pass
    return _load_mido(path)


def _load_symusic(path):
    return score_notes(symusic.Score(str(path)))


def score_notes(score):
    q = 4.0 / score.ticks_per_quarter
    notes = []
    for ti, tr in enumerate(score.tracks):
        if tr.is_drum:
            continue
        for n in tr.notes:
            if n.duration > 0:
                notes.append(Note(n.time * q, (n.time + n.duration) * q, n.pitch, (ti, 0)))
    ts = [(t.time * q, t.numerator, t.denominator) for t in score.time_signatures]
    return notes, _clean_ts(ts)


def _clean_ts(ts):
    ts = sorted({p: (p, n, d) for p, n, d in sorted(ts) if n > 0 and d > 0}.values()) or [(0.0, 4, 4)]
    if ts[0][0] > 0:
        ts.insert(0, (0.0, 4, 4))
    return ts


def _load_mido(path):
    mid = mido.MidiFile(path)
    q = 4.0 / mid.ticks_per_beat
    notes, ts = [], []
    for ti, track in enumerate(mid.tracks):
        now, active = 0, {}
        for msg in track:
            now += msg.time
            if msg.type == "time_signature":
                ts.append((now * q, msg.numerator, msg.denominator))
            elif msg.type == "note_on" and msg.velocity > 0:
                active.setdefault((msg.channel, msg.note), []).append(now)
            elif msg.type == "note_off" or msg.type == "note_on":
                starts = active.get((msg.channel, msg.note))
                if starts:
                    s = starts.pop(0)
                    if msg.channel != 9 and now > s:
                        notes.append(Note(s * q, now * q, msg.note, (ti, msg.channel)))
    return notes, _clean_ts(ts)


MELODY_VOICE_WEIGHT = 0.25


def mark_melody(notes):
    if not notes:
        return None
    by_voice = {}
    for n in notes:
        by_voice.setdefault(n.voice, []).append(n)
    median = float(np.median([n.pitch for n in notes]))
    found = False
    if len(by_voice) > 1:
        for vs in by_voice.values():
            vs.sort(key=lambda n: n.start)
            overlaps = sum(1 for a, b in zip(vs, vs[1:]) if b.start < a.end - 0.5)
            mono = 1 - overlaps / max(len(vs) - 1, 1)
            mean = float(np.mean([n.pitch for n in vs]))
            if mono >= 0.85 and mean > median + 2 and mean >= 60:
                for n in vs:
                    n.melody = True
                found = True
    if not found:
        by_onset = {}
        for n in notes:
            by_onset.setdefault(round(n.start, 2), []).append(n)
        active = []
        for t in sorted(by_onset):
            ns = by_onset[t]
            active = [m for m in active if m.end > t]
            top = max(ns, key=lambda n: n.pitch)
            others = [m.pitch for m in active] + [m.pitch for m in ns if m is not top]
            if top.pitch >= 60 and (not others or top.pitch - max(others) >= 5):
                top.melody = True
            active.extend(ns)
        return "skyline"
    return "voice"


MIN_GRID16 = 4


def chord_step(num, den):
    bar = num * 16.0 / den
    unit = 16.0 / den
    beat = unit * 3 if (num % 3 == 0 and num > 3 and den >= 8) else unit
    step = beat
    while step < MIN_GRID16 and step < bar:
        step += beat
        while bar % step > 1e-6 and step < bar:
            step += beat
    return min(step, bar)


def clean_meters(ts, end):
    out = []
    for k, (p, num, den) in enumerate(ts):
        nxt = ts[k + 1][0] if k + 1 < len(ts) else end
        bar = num * 16.0 / den
        if num == 1 and den >= 8 and nxt - p > 2 * bar:
            num, den = 4, 4
        out.append((p, num, den))
    return out


def bar_grid(ts, end):
    ts = clean_meters(ts, end)
    bars, i, cur = [], 0, 0.0
    while cur < end:
        while i + 1 < len(ts) and ts[i + 1][0] <= cur + 1e-6:
            i += 1
        _, num, den = ts[i]
        bar_len = num * 16.0 / den
        for sub in _split_bar(num, den):
            sub_len = sub * 16.0 / den
            bars.append((cur, sub_len, chord_step(sub, den)))
            cur += sub_len
    return bars


MAX_BAR16 = 64


def _split_bar(num, den):
    if num * 16.0 / den <= MAX_BAR16:
        return [num]
    k = -(-int(num * 16 // den) // MAX_BAR16)
    for kk in range(k, num // 2 + 1):
        if num % kk == 0 and (num // kk) * 16.0 / den <= MAX_BAR16:
            return [num // kk] * kk
    base, extra = divmod(num, k)
    while (base + (extra > 0)) * 16.0 / den > MAX_BAR16:
        k += 1
        base, extra = divmod(num, k)
    return [base + 1] * extra + [base] * (k - extra)


def histograms(notes, bounds, bass_boost=True):
    starts = [b[0] for b in bounds]
    hist = np.zeros((len(bounds), 12))
    low = [128] * len(bounds)
    for n in notes:
        i = max(bisect.bisect_right(starts, n.start) - 1, 0)
        while i < len(bounds) and bounds[i][0] < n.end:
            a, b = bounds[i]
            ov = min(b, n.end) - max(a, n.start)
            if ov > 0:
                w = ov
                if bass_boost:
                    if n.melody:
                        w *= MELODY_VOICE_WEIGHT
                    else:
                        w *= 1.5 if n.pitch < 60 else MELODY_WEIGHT if n.pitch >= MELODY_PITCH else 1.0
                hist[i, n.pitch % 12] += w
                if ov >= 0.25 * (b - a):
                    low[i] = min(low[i], n.pitch)
            i += 1
    return hist, low


def _zscore(x):
    x = x - x.mean(-1, keepdims=True)
    return x / np.sqrt((x * x).sum(-1, keepdims=True))


_KEY_Z = _zscore(np.array([np.roll(AARDEN_MAJ, t) for t in range(12)]
                          + [np.roll(AARDEN_MIN, t) for t in range(12)]))

KS_MAJ = np.array([6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88])
KS_MIN = np.array([6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17])
_KEY_Z_KS = _zscore(np.array([np.roll(KS_MAJ, t) for t in range(12)]
                             + [np.roll(KS_MIN, t) for t in range(12)]))
_KEY_TABLES = {"aarden": _KEY_Z, "ks": _KEY_Z_KS}


def _key_scores(h, profile="aarden"):
    if h.sum() <= 0:
        return None
    hz = h - h.mean()
    norm = np.sqrt((hz * hz).sum())
    if norm == 0:
        return np.full(24, np.nan)
    return _KEY_TABLES[profile] @ (hz / norm)


def _resolve_relative(scores, h, k, ev=None):
    rel = (k + 9) % 12 + 12 if k < 12 else (k - 12 + 3) % 12
    if abs(scores[k] - scores[rel]) > 0.03:
        return k
    minor = k if k >= 12 else rel
    major = rel if k >= 12 else k
    t = minor - 12
    if ev is not None and abs(ev[t] - ev[major]) > 1e-6:
        return minor if ev[t] > ev[major] else major
    leading, subtonic = h[(t + 11) % 12], h[(t + 10) % 12]
    return minor if leading > subtonic else major


CLOSURE_INTERVALS = (5, 7, 10)


def _key_evidence(segs, chords, low, n_bars, closure):
    ev = np.zeros((n_bars, 12))
    runs = []
    for i, (a, b, bi) in enumerate(segs):
        d = b - a
        if low[i] < 128:
            ev[bi, low[i] % 12] += d
        c = chords[i]
        if c is None or c == SOLO:
            continue
        ev[bi, c[0]] += d
        if runs and runs[-1][0] == c[0]:
            runs[-1][1] += d
        else:
            runs.append([c[0], d, bi])
    if closure:
        for j in range(1, len(runs)):
            r, d, bi = runs[j]
            step = (runs[j - 1][0] - r) % 12
            if step in CLOSURE_INTERVALS:
                ev[bi, r] += d
                if step == 10 and j >= 2 and (runs[j - 2][0] - r) % 12 == 8:
                    ev[bi, r] += d
    return ev


def _chord_tone_histogram(segs, chords, n_bars):
    h = np.zeros((n_bars, 12))
    for (a, b, bi), c in zip(segs, chords):
        if c is None or c == SOLO:
            continue
        root, ti = c[0], c[1]
        for iv in TEMPLATES[ti][1]:
            h[bi, (root + iv) % 12] += b - a
    return h


def estimate_keys(notes, bars, window=8, hold=4, global_prior=0.05, evidence=None,
                  profile="aarden", hist=None):
    bounds = [(s, s + l) for s, l, _ in bars]
    if hist is None:
        hist, _ = histograms(notes, bounds, bass_boost=False)
    g = _key_scores(hist.sum(0), profile)
    if g is None:
        return None
    gk = _resolve_relative(g, hist.sum(0), int(np.argmax(g)),
                           None if evidence is None else evidence.sum(0))
    raw = []
    half = window // 2
    for i in range(len(bars)):
        lo = max(0, min(i - half, len(bars) - window))
        h = hist[lo:lo + window].sum(0)
        s = _key_scores(h, profile)
        if s is None:
            raw.append(raw[-1] if raw else gk)
            continue
        s[gk] += global_prior
        ev = None if evidence is None else evidence[lo:lo + window].sum(0)
        raw.append(_resolve_relative(s, h, int(np.argmax(s)), ev))
    keys, cur, run, cand = [], raw[0], 0, None
    for i, k in enumerate(raw):
        if k == cur:
            run, cand = 0, None
        elif k == cand:
            run += 1
            if run >= hold:
                cur = k
                for j in range(i - hold + 1, i):
                    keys[j] = cur
        else:
            cand, run = k, 1
            if hold <= 1:
                cur = k
        keys.append(cur)
    return keys


_T = len(TEMPLATES)
_FIT_M = np.zeros((12 * _T, 12))
_FIT_ROOT = np.repeat(np.arange(12), _T)
_FIT_EXTRA = np.zeros(12 * _T)
for _r in range(12):
    for _ti, (_, _iv) in enumerate(TEMPLATES):
        _FIT_M[_r * _T + _ti, [(_r + i) % 12 for i in _iv]] = 1
        _FIT_EXTRA[_r * _T + _ti] = EXTRA_TONE_PENALTY * max(0, len(_iv) - 3)


def fit_chord(h, lowest, roots=None):
    tot = h.sum()
    if tot < MIN_SEGMENT_WEIGHT:
        return None
    w = h / tot
    s = 2 * (_FIT_M @ w) - 1 - MISS_PENALTY * (_FIT_M @ (w < 0.03)) - _FIT_EXTRA
    if lowest < 128:
        s = s + BASS_ROOT_BONUS * (_FIT_ROOT == lowest % 12)
    if roots is not None:
        s = np.where(np.isin(_FIT_ROOT, list(roots)), s, -np.inf)
    k = int(np.argmax(s >= s.max() - 1e-9))
    return int(_FIT_ROOT[k]), k % _T


def _mask(iv):
    m = 0
    for i in iv:
        if i:
            m |= 1 << (i - 1)
    return m


NO_ABSORB_DEGREES = frozenset((7, 11))
_ABSORB_HIT = Counter()
_ABSORB_KEPT = Counter()

PEDAL_MIN16 = 32


def _pedal_refit(notes, segs, bars, hfit, low, chords):
    srt = sorted(notes, key=lambda n: n.start)
    starts = [n.start for n in srt]
    longest = max((n.end - n.start for n in srt), default=0)
    i = 0
    while i < len(segs):
        if low[i] >= 128:
            i += 1
            continue
        j = i
        while j + 1 < len(segs) and low[j + 1] == low[i]:
            j += 1
        if segs[j][1] - segs[i][0] >= PEDAL_MIN16:
            pitch = low[i]
            a, b = segs[i][0], segs[j][1]
            lo = bisect.bisect_left(starts, a - longest)
            hi = bisect.bisect_left(starts, b)
            upper = [n for n in srt[lo:hi] if n.pitch != pitch and n.end > a]
            half = lambda k: (segs[k][2], int((segs[k][0] - bars[segs[k][2]][0]) * 2 // bars[segs[k][2]][1]))
            groups = []
            for k in range(i, j + 1):
                if groups and half(groups[-1][0]) == half(k):
                    groups[-1].append(k)
                else:
                    groups.append([k])
            h_up, _ = histograms(upper, [(segs[k][0], segs[k][1]) for k in range(i, j + 1)])
            for k in range(i, j + 1):
                hfit[k] = h_up[k - i]
            for g in groups:
                fit = fit_chord(sum(hfit[k] for k in g), 128)
                if fit is None:
                    continue
                root, ti = fit
                for k in g:
                    chords[k] = (root, ti, (pitch % 12 - root) % 12)
        i = j + 1


def _mark_solo(notes, segs, chords, melody_mode):
    bounds = [(a, b) for a, b, _ in segs]
    if melody_mode == "voice":
        acc = [n for n in notes if not n.melody]
        if SOLO_MIN_ACC_NOTES * len(notes) <= len(acc) < len(notes):
            h_acc, _ = histograms(acc, bounds, bass_boost=False)
            for i in range(len(segs)):
                if chords[i] is not None and h_acc[i].sum() < SOLO_ACC_MIN16:
                    chords[i] = SOLO
    srt = sorted(notes, key=lambda n: n.start)
    starts = [n.start for n in srt]
    longest = max((n.end - n.start for n in srt), default=0)
    for i, (a, b) in enumerate(bounds):
        if chords[i] is None or chords[i] == SOLO:
            continue
        lo = bisect.bisect_left(starts, a - longest)
        hi = bisect.bisect_left(starts, b)
        ns = sorted((n for n in srt[lo:hi] if n.end > a), key=lambda n: n.start)
        if not ns or len({n.pitch % 12 for n in ns}) >= 3:
            continue
        if all(y.start >= x.end - 0.5 for x, y in zip(ns, ns[1:])):
            chords[i] = SOLO


def encode(path, **opts):
    return encode_notes(*load(path), **opts)


def encode_score(score, **opts):
    return encode_notes(*score_notes(score), **opts)


REMI_BOS, REMI_EOS, REMI_FIRST_MUSIC_ID = 1, 2, 4


def remi_doc_ids(ids):
    d = np.asarray(ids)
    d = d[1:] if len(d) and d[0] == REMI_BOS else d
    cut = np.nonzero(d == REMI_EOS)[0]
    d = d[:cut[0]] if len(cut) else d
    return d[d >= REMI_FIRST_MUSIC_ID].astype(np.int64).tolist()


def encode_remi(ids, tokenizer, **opts):
    return encode_score(tokenizer.decode(remi_doc_ids(ids)), **opts)


_BASE_CACHE = {}


def _base_table(tokenizer):
    key = id(tokenizer)
    if key not in _BASE_CACHE:
        m, lookup = tokenizer._model, tokenizer._vocab_learned_bytes_to_tokens
        _BASE_CACHE[key] = [tuple(lookup.get(m.id_to_token(i), ())) if i >= REMI_FIRST_MUSIC_ID else ()
                            for i in range(len(tokenizer))]
    return _BASE_CACHE[key]


def remi_times(ids, tokenizer, with_notes=False):
    from miditok.utils import compute_ticks_per_bar
    from symusic import TimeSignature
    base = _base_table(tokenizer)
    ids = np.asarray(ids)
    cut = np.nonzero(ids == REMI_EOS)[0]
    n_music = int(cut[0]) if len(cut) else len(ids)
    flat, owner = [], []
    for k in range(n_music):
        for b in base[int(ids[k])]:
            flat.append(b)
            owner.append(k)
    cfg, tpq = tokenizer.config, tokenizer.time_division
    dur_offset = 2 if cfg.use_velocities else 1

    num, den = 4, 4
    for b in flat:
        typ = b.split("_")[0]
        if typ == "TimeSig":
            num, den = tokenizer._parse_token_time_signature(b.split("_")[1])
            break
        if typ in ("Pitch", "PitchDrum", "Velocity", "Duration", "PitchBend", "Pedal"):
            break

    def meter(n, d):
        tpb = tokenizer._tpb_per_ts[d]
        return (compute_ticks_per_bar(TimeSignature(0, n, d), tpq), tpb,
                tpb // cfg.max_num_pos_per_beat)

    ticks_per_bar, tpb, ticks_per_pos = meter(num, den)
    tick = at_bar = at_ts = 0
    bar, bar_at_ts, note_end = -1, 0, 0
    out = np.zeros(len(ids), dtype=np.float64)
    has_note = np.zeros(len(ids), dtype=bool)
    has_rest = np.zeros(len(ids), dtype=bool)
    j = 0
    for k in range(n_music):
        while j < len(flat) and owner[j] == k:
            b = flat[j]
            typ, val = b.split("_", 1)
            if b == "Bar_None":
                bar += 1
                if bar > 0:
                    tick = at_bar + ticks_per_bar
                at_bar = tick
            elif typ == "Rest":
                has_rest[k] = True
                tick = max(note_end, tick) + tokenizer._tpb_rests_to_ticks[tpb][val]
                real_bar = bar_at_ts + (tick - at_ts) // ticks_per_bar
                if real_bar > bar:
                    if bar == -1:
                        bar = 0
                    at_bar += (real_bar - bar) * ticks_per_bar
                    bar = real_bar
            elif typ == "Position":
                if bar == -1:
                    bar = 0
                tick = at_bar + int(val) * ticks_per_pos
            elif typ in ("Pitch", "PitchDrum"):
                has_note[k] = True
                if j + dur_offset < len(flat) and flat[j + dur_offset].startswith("Duration_"):
                    d = tokenizer._tpb_tokens_to_ticks[tpb][flat[j + dur_offset].split("_", 1)[1]]
                    note_end = max(note_end, tick + d)
            elif typ == "Tempo":
                note_end = max(note_end, tick)
            elif typ == "TimeSig":
                n2, d2 = tokenizer._parse_token_time_signature(val)
                if (n2, d2) != (num, den):
                    num, den = n2, d2
                    at_ts, bar_at_ts = at_bar, bar
                    ticks_per_bar, tpb, ticks_per_pos = meter(num, den)
            j += 1
        out[k] = tick * 4.0 / tpq
    out[n_music:] = out[n_music - 1] if n_music else 0.0
    return (out, has_note, has_rest) if with_notes else out


def chords_between(tokens, t0, t1, closed_end=True):
    if closed_end:
        return [t for t in tokens if t.start16 <= t1 and t.start16 + t.dur > t0]
    return [t for t in tokens if t.start16 < t1 and t.start16 + t.dur > t0]


def _window_info(chords, t0):
    first = chords[0] if chords else None
    return dict(
        chords=np.array([t.packed for t in chords], dtype=np.uint64),
        first_bar=first.bar if first else None,
        mid_piece=bool(first and (first.bar > 0 or first.onset > 0)),
        clipped=bool(first and first.start16 >= 0 and first.start16 < t0),
    )


def windows(stream, size, stride=None, tokenizer=None, **opts):
    stride = stride or size
    if tokenizer is None:
        packed = np.asarray([t.packed for t in stream] if stream and isinstance(stream[0], Token)
                            else stream, dtype=np.uint64)
        for s in range(0, max(len(packed), 1), stride):
            w = packed[s:s + size]
            if not len(w):
                break
            first = unpack(int(w[0]))
            yield dict(start=s, end=s + len(w), chords=w, first_bar=first.bar,
                       mid_piece=first.bar > 0 or first.onset > 0, clipped=False)
            if s + size >= len(packed):
                break
        return
    ids = np.asarray(stream)
    chords = encode_score(tokenizer.decode(remi_doc_ids(ids)), **opts)
    t, has_note, has_rest = remi_times(ids, tokenizer, with_notes=True)
    for s in range(0, max(len(ids), 1), stride):
        e = min(s + size, len(ids))
        nb = np.nonzero(has_note[s:e])[0]
        if len(nb):
            t0 = float(t[s + nb[0]])
            rp = np.nonzero(has_rest[s:e])[0]
            t_note = float(t[s + nb[-1]])
            t_rest = float(t[s + rp[-1]]) if len(rp) else -1.0
            t1 = max(t_note, t_rest)
            closed = t_rest <= t_note
        else:
            rp = np.nonzero(has_rest[s:e])[0]
            if len(rp):
                i0 = s + rp[0]
                t0 = float(t[i0 - 1]) if i0 > 0 else 0.0
                t1 = float(t[s + rp[-1]])
                closed = False
            else:
                t0 = t1 = float(t[e - 1]) if e > 0 else 0.0
                closed = True
        info = _window_info(chords_between(chords, t0, t1, closed_end=closed), t0)
        yield dict(start=s, end=e, remi=ids[s:e], t0=float(t0), t1=float(t1), **info)
        if e >= len(ids):
            break


TRIAD_OF = {i: (0 if name == "add9" else 1) for i, (name, _) in enumerate(TEMPLATES)
            if name in ("add9", "madd9")}


def encode_notes(notes, ts, smooth=False, merge_root=True, pedal=True, melody=True, solo=True,
                 key_evidence="closure", add9=False, key_profile="aarden", key_hist="notes"):
    if not notes:
        return []
    if key_evidence not in ("none", "root_bass", "closure"):
        raise ValueError(f"key_evidence must be none, root_bass or closure, not {key_evidence!r}")
    if key_profile not in ("aarden", "ks") or key_hist not in ("notes", "chords"):
        raise ValueError(f"key_profile must be aarden or ks and key_hist notes or chords")
    melody_mode = mark_melody(notes) if melody else None
    end = max(n.end for n in notes)
    bars = bar_grid(ts, end)

    segs = []
    for bi, (s, l, beat) in enumerate(bars):
        t = s
        while t < s + l - 1e-6:
            segs.append((t, min(t + beat, s + l), bi))
            t += beat
    hist, low = histograms(notes, [(a, b) for a, b, _ in segs])
    hfit = hist.copy()
    chords = []
    for i, (a, b, bi) in enumerate(segs):
        fit = fit_chord(hist[i], low[i])
        if fit is None:
            chords.append(None)
        else:
            root, ti = fit
            bass = (low[i] % 12 - root) % 12 if low[i] < 128 else 0
            chords.append((root, ti, bass))
    if pedal:
        _pedal_refit(notes, segs, bars, hfit, low, chords)
    if solo:
        _mark_solo(notes, segs, chords, melody_mode)
    if not add9:
        for i, c in enumerate(chords):
            if c is not None and c != SOLO and c[1] in TRIAD_OF:
                chords[i] = (c[0], TRIAD_OF[c[1]], c[2])

    evidence = None if key_evidence == "none" else _key_evidence(
        segs, chords, low, len(bars), closure=key_evidence == "closure")
    keys = estimate_keys(notes, bars, evidence=evidence, profile=key_profile,
                         hist=_chord_tone_histogram(segs, chords, len(bars)) if key_hist == "chords" else None)
    if keys is None:
        return []

    if smooth:
        for i in range(1, len(chords) - 1):
            if chords[i] is None or chords[i] == SOLO or chords[i] == chords[i - 1]:
                continue
            if chords[i - 1] != chords[i + 1]:
                continue
            if chords[i - 1] is None or chords[i - 1] == SOLO:
                continue
            tonic = keys[segs[i][2]] % 12
            degree = (chords[i][0] - tonic) % 12
            nb = chords[i - 1]
            nb_degree = None if nb is None or nb == SOLO else (nb[0] - tonic) % 12
            if degree in NO_ABSORB_DEGREES or nb_degree in NO_ABSORB_DEGREES:
                _ABSORB_KEPT[degree] += 1
                continue
            _ABSORB_HIT[degree] += 1
            chords[i] = chords[i - 1]

    root_of = lambda c: c if c is None or c == SOLO else c[0]
    same = (lambda x, y: root_of(x) == root_of(y)) if merge_root else (lambda x, y: x == y)
    tokens = []
    i = 0
    while i < len(segs):
        j = i
        while (j + 1 < len(segs) and same(chords[j + 1], chords[i])
               and keys[segs[j + 1][2]] == keys[segs[i][2]]):
            j += 1
        start, stop, bi = segs[i][0], segs[j][1], segs[i][2]
        k = keys[bi]
        tonic, mode = k % 12, int(k >= 12)
        c = chords[i]
        if c is not None and c != SOLO and merge_root and j > i:
            h = hfit[i:j + 1].sum(0)
            lo = min(low[i:j + 1])
            refit = fit_chord(h, lo, roots=[c[0]])
            if refit is not None:
                root, ti = refit
                if not add9 and ti in TRIAD_OF:
                    ti = TRIAD_OF[ti]
                c = (root, ti, (lo % 12 - root) % 12 if lo < 128 else 0)
        if c is None:
            degree, mask, bass, label = NC_DEGREE, 0, 0, "N.C."
        elif c == SOLO:
            degree, mask, bass, label = SOLO_DEGREE, 0, 0, "solo"
        else:
            root, ti, bass = c
            name, iv = TEMPLATES[ti]
            degree, mask = (root - tonic) % 12, _mask(iv)
            label = PC[root] + name + (f"/{PC[(root + bass) % 12]}" if bass else "")
        _emit(tokens, bars, start, stop, tonic, mode, degree, mask, bass, label)
        i = j + 1
    while tokens and tokens[-1].is_silence:
        tokens.pop()
    return tokens


def _emit(tokens, bars, start, stop, tonic, mode, degree, mask, bass, label):
    starts = [b[0] for b in bars]
    cont = 0
    while stop - start > 1e-6:
        bi = bisect.bisect_right(starts, start + 1e-6) - 1
        if bi > 65535:
            return
        step = min(stop - start, 255.0)
        k = bi + 1
        while k < len(bars) and bars[k][0] < start + step - 1e-6:
            if round(bars[k][1]) != round(bars[bi][1]):
                step = bars[k][0] - start
                break
            k += 1
        onset = min(int(round(start - bars[bi][0])), 63)
        barlen = min(max(int(round(bars[bi][1])), 1), 64)
        tokens.append(Token(tonic, mode, degree, mask, bass, bi, onset,
                            max(int(round(step)), 1), barlen, cont, label, start16=start))
        start += step
        cont = 1


def rope_inputs(tokens):
    pos = np.array([t.bar * 16 + t.onset for t in tokens], dtype=np.int32)
    root = np.array([-1 if t.is_nc else t.root_pc for t in tokens], dtype=np.int8)
    tonic = np.array([t.tonic for t in tokens], dtype=np.int8)
    return pos, root, tonic


def to_array(tokens):
    return np.array([t.packed for t in tokens], dtype=np.uint64)


ROMAN = ["I", "bII", "II", "bIII", "III", "IV", "bV", "V", "bVI", "VI", "bVII", "VII"]


def roman(t):
    if t.is_nc:
        return "solo" if t.is_solo else "N.C."
    r = ROMAN[t.degree]
    minorish = t.mask & (1 << 2) and not t.mask & (1 << 3)
    return r.lower() if minorish else r


def show(path, tokens, window=None):
    print(f"\n{path}  ({len(tokens)} tokens, {len(tokens) * 8} bytes)")
    rows = tokens if window is None else tokens[window[0]:window[0] + window[1]]
    base = 0 if window is None else window[0]
    if window is not None and rows:
        f = rows[0]
        print(f"window tokens {window[0]}..{window[0] + len(rows) - 1}: "
              f"starts mid-piece = {f.bar > 0 or f.onset > 0} (bar {f.bar}, onset {f.onset})")
    print(f"{'#':>4} {'bar':>4}.{'on':<2} {'dur':>4}  {'len':>3} {'key':<4} {'rn':<6} "
          f"{'chord':<10} {'packed':>14}")
    for i, t in enumerate(rows):
        key = PC[t.tonic] + ("m" if t.mode else "")
        c = "+" if t.cont else " "
        print(f"{base + i:>4} {t.bar:>4}.{t.onset:<2} {t.dur:>4}{c} {t.barlen:>3} {key:<4} "
              f"{roman(t):<6} {t.label:<10} {t.packed:#014x}")


EPILOG = """\
examples:
  Encode MIDI files and print every chord token:
    python chord_tokens.py song.mid other.mid

  Print only tokens 30..41 of a file (a token window):
    python chord_tokens.py song.mid --window 30 12

  Encode documents straight from a REMI training cache (no MIDI file needed):
    python chord_tokens.py --remi unified_packed_cond.npz --doc 214824 212410

  Cut a REMI document into back-to-back training-style windows of 1024 REMI tokens,
  and print each window's chords:
    python chord_tokens.py --remi unified_packed_cond.npz --doc 214824 --windows 1024

  Same, but windows that overlap by half (a new window every 512 tokens):
    python chord_tokens.py --remi unified_packed_cond.npz --doc 214824 --windows 1024 --stride 512

  Same, but with melody notes counted at full weight:
    python chord_tokens.py --remi unified_packed_cond.npz --doc 214824 --windows 1024 --no-melody

windowing, in short:
  --windows SIZE    how many REMI tokens each window holds.
  --stride STRIDE   how far the window start moves each time. STRIDE = SIZE (the default)
                    gives back-to-back windows with no overlap; STRIDE < SIZE gives
                    overlapping windows (512 with SIZE 1024 means each token appears in
                    two windows); STRIDE > SIZE skips tokens between windows. The last
                    window may be shorter than SIZE.
  --window counts CHORD tokens (a slice of the encoded output); --windows counts REMI
  tokens (positions in the training stream) and reports the chords sounding in each.

output columns:
  #        token index           bar.on   bar number . 16ths after the downbeat
  dur      length in 16ths (+ = continues the previous token, a split long chord)
  len      length of this token's bar in 16ths (the effective bar, not the signature)
  key      estimated key         rn       Roman numeral in that key (solo / N.C. if none)
  chord    chord name            packed   the 64-bit token as hex
  --windows lines: [start, end) REMI positions, t = music time covered in 16ths,
  bar = first chord's bar, mid = window starts after the piece start,
  clip = the first chord began before the window (it is included whole).
"""


def main():
    ap = argparse.ArgumentParser(
        prog="chord_tokens.py",
        description=("ChordStream: one 64-bit token per chord change or silence, carrying key, "
                     "degree, pitch-class mask, bass, bar, onset, bar length and duration."),
        epilog=EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter)

    src = ap.add_argument_group("input: MIDI files")
    src.add_argument("midi", nargs="*", metavar="FILE.mid",
                     help="one or more MIDI files to encode and print")

    remi = ap.add_argument_group("input: REMI token cache (tokenised MIDI, no files needed)")
    remi.add_argument("--remi", metavar="CACHE.npz",
                      help="packed REMI cache holding tokens.npy and doc_start.npy "
                           "(e.g. unified_packed_cond.npz)")
    remi.add_argument("--doc", type=int, nargs="+", default=[], metavar="N",
                      help="document number(s) to read from --remi (0-based)")
    remi.add_argument("--tokenizer", default="tokenizer.json", metavar="PATH",
                      help="miditok REMI tokenizer the cache was built with (default: %(default)s)")

    out = ap.add_argument_group("windowing and output")
    out.add_argument("--window", nargs=2, type=int, metavar=("START", "N"),
                     help="print only N chord tokens starting at chord token START "
                          "(a slice of the encoded output)")
    out.add_argument("--windows", type=int, metavar="SIZE",
                     help="with --remi: cut each document into windows of SIZE REMI tokens and "
                          "print the chords sounding in each. See 'windowing' below")
    out.add_argument("--stride", type=int, metavar="STRIDE",
                     help="with --windows: start a new window every STRIDE REMI tokens "
                          "(default: STRIDE = SIZE, back-to-back windows with no overlap)")

    enc = ap.add_argument_group("encoder switches (all on by default; these turn one off)")
    enc.add_argument("--no-melody", action="store_true",
                     help="count melody voices at full weight instead of 0.25 when fitting chords "
                          "(also disables the melody-only solo rule)")
    enc.add_argument("--no-pedal", action="store_true",
                     help="don't detect pedal points (a bass held 2+ bars under moving harmony)")
    enc.add_argument("--beat-level", action="store_true",
                     help="keep one token per chord step: don't merge consecutive steps on the "
                          "same root (Cm, Cm7, Cmadd9 stay separate)")
    enc.add_argument("--smooth", action="store_true",
                     help="absorb one-step chords between two identical chords (off by default: "
                          "it erased real harmony at no measured gain)")
    enc.add_argument("--add9", action="store_true",
                     help="keep add9 and madd9 readings instead of emitting their triads")
    enc.add_argument("--key-evidence", choices=["none", "root_bass", "closure"], default="closure",
                     help="how relative major/minor near-ties are settled (default closure)")
    a = ap.parse_args()
    if (a.doc or a.windows is not None) and not a.remi:
        ap.error("--doc and --windows read from a REMI cache: add --remi CACHE.npz")
    if a.remi and not a.doc:
        ap.error("--remi needs --doc N [N ...]")
    if not a.midi and not a.remi:
        ap.error("give MIDI files, or --remi CACHE.npz --doc N")
    if a.stride is not None and a.windows is None:
        ap.error("--stride only applies with --windows SIZE")
    if (a.windows is not None and a.windows < 1) or (a.stride is not None and a.stride < 1):
        ap.error("--windows SIZE and --stride STRIDE must be positive integers")
    opts = dict(smooth=a.smooth, merge_root=not a.beat_level, pedal=not a.no_pedal,
                melody=not a.no_melody, add9=a.add9, key_evidence=a.key_evidence)
    for p in a.midi:
        toks = encode(p, **opts)
        assert all(unpack(t.packed).packed == t.packed for t in toks)
        show(p, toks, a.window)
    if a.remi:
        from miditok import REMI
        tok = REMI(params=a.tokenizer)
        cache = np.load(a.remi)
        stream = cache["tokens"]
        starts = np.asarray(cache["doc_start"])
        for i in a.doc:
            end = int(starts[i + 1]) if i + 1 < len(starts) else len(stream)
            if a.windows is not None:
                size, stride = a.windows, a.stride
                print(f"\n{a.remi} doc {i}: REMI windows of {size}, stride {stride or size}")
                for w in windows(stream[int(starts[i]):end], size, stride, tokenizer=tok, **opts):
                    names = " ".join(roman(unpack(int(v))) for v in w["chords"][:12])
                    print(f"  [{w['start']:6}, {w['end']:6})  t {w['t0']:8.1f}-{w['t1']:8.1f}  "
                          f"bar {w['first_bar']}  mid {w['mid_piece']!s:5}  clip {w['clipped']!s:5}  "
                          f"{len(w['chords']):3} chords: {names}")
                continue
            toks = encode_remi(stream[int(starts[i]):end], tok, **opts)
            show(f"{a.remi} doc {i}", toks, a.window)


if __name__ == "__main__":
    main()
