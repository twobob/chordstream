# ChordStream 0.2

ChordStream is a compact symbolic representation of harmony for training music models: one 64-bit
token per chord change or silence, carrying the key, the chord's degree relative to that key, its
pitch-class content, its bass, and its bar, onset, bar length and duration in musical time. It is
deterministic, invariant to tempo, and self-describing under windowing: any slice of tokens states its
own bar, key and bar length.

This repository holds the specification, `chordstream.pdf`, written so that the representation can be
re-implemented from it alone, and two implementations of it:

* `chordstream_tokens.py`, the Python reference
* `chordstream.c`, a single-file C implementation using only the standard library, which reproduces
  the Python token for token under `--ref` and is several times faster

## What is new in 0.2

* The C implementation.
* A tuned configuration in both implementations (below), measured on held-out test sets.
* A specification revised after an independent clean-room implementation: note order, the bar-line rule
  for time signatures, the melody fallback's onset grouping and the tolerance of solo rule B are now
  stated exactly.

## Python

Python 3.11 (the version it was tested on), with `numpy` and `mido`. `symusic` is used when installed
and makes MIDI loading much faster. Reading REMI token streams also needs `miditok`; results here used
miditok 3.0.6 and symusic 0.6.0.

```
pip install numpy mido symusic miditok
python chordstream_tokens.py song.mid
python chordstream_tokens.py --tuned song.mid
```

```python
import chordstream_tokens as ct

tokens = ct.encode("song.mid")                # the specification's configuration
tuned = ct.encode("song.mid", tuned=True)     # the tuned configuration
packed = ct.to_array(tokens)                  # numpy uint64, one value per token
same = ct.unpack(int(packed[0]))              # a Token back from its 64 bits
```

`python chordstream_tokens.py --help` lists every option, including reading documents from a REMI token
cache and cutting them into windows.

## C

```
cl /std:c11 /W4 /WX /O2 chordstream.c                                    (MSVC)
gcc -std=c99 -O2 -Wall -Wextra -pedantic chordstream.c -lm -o chordstream
```

Built and tested with MSVC 14.43; the gcc line is given but was not tested for this release.

```
chordstream                          run the 15 specification tests
chordstream --ref song.mid           the Python reference's output, token for token
chordstream --ref --tuned song.mid   the Python reference's tuned output, token for token
chordstream --tsv --ref song.mid     machine-readable token table
chordstream notes.json               notes in sixteenths: {"notes": [[start, end, pitch, voice], ...],
                                     "ts": [[start, numerator, denominator], ...]}
```

Without `--ref` the C follows its own literal reading of the specification, which differs from the Python
at a few edge cases; `--ref` is the mode to use when results must match. `--help` lists the remaining
options, including `--opt name=value` for the individual constants.

Agreement with the Python, every file of three sets (907 pop, 949 long non-pop, 150 classical):

| | bit-identical token streams |
|---|---|
| `--ref` against Python | 2,004 of 2,006 |
| `--ref --tuned` against Python tuned | 2,006 of 2,006 |

The two exceptions are keys the Python itself decides by floating-point rounding, where two keys score
equal to within 1e-16, which no independent arithmetic reproduces.

Speed on one core, same files: pop pieces 3.1 ms per file in C against 27.5 ms in Python; long non-pop
pieces 20.8 ms against 90.6 ms.

## Tuned configuration

The tuned configuration changes four things from the specification, chosen on the 816 POP909-CL training
pieces and never on the test pieces:

1. The penalty on tones beyond a triad rises from 0.06 to 0.25, since the specification emits too many
   sevenths against human labels.
2. A winning power chord is named as a triad: the third with more weight, or failing that the key's
   diatonic third.
3. The key window grows from 8 to 16 bars, with 6 bars of hysteresis instead of 4.
4. Key profiles fitted on the training pieces replace Aarden-Essen; the fitted minor profile is Aeolian.

Change 2 consults the key when neither third sounds, so in the tuned configuration chord fitting is no
longer entirely independent of key inference, as it is in the specification.

## How well it works

Measured against human harmonic annotation with the evaluation protocol and published test splits of
BACHI. Percentages; keys are MIREX weighted and exact.

| POP909-CL, pop, 91 pieces | root | quality | bass | full chord | key MIREX | key exact |
|---|---|---|---|---|---|---|
| specification | 86.0 | 70.4 | 91.2 | 67.3 | 85.1 | 79.7 |
| tuned | 90.0 | 82.7 | 91.6 | 78.7 | 88.3 | 83.9 |
| BACHI (learned) | 93.3 | 88.2 | 95.2 | 86.1 | | |

| When in Rome and DCML, classical, 150 pieces | root | quality | bass | full chord | key MIREX | key exact |
|---|---|---|---|---|---|---|
| specification | 60.9 | 52.5 | 61.4 | 39.1 | 72.1 | 62.9 |
| tuned | 60.0 | 53.3 | 61.4 | 39.0 | 72.4 | 62.6 |
| BACHI, classical model (learned) | 78.2 | 79.1 | 77.5 | 68.5 | | |

On pop the tuned configuration closes more than half the gap to a learned recogniser. On classical music
it neither helps nor hurts, and both configurations sit well behind a model trained on that repertoire.
The paper gives the full evaluation and the known limitations.

## Token layout

Two 32-bit words, `a | (b << 32)`, least significant bit first:

| word | field | bits | meaning |
|---|---|---|---|
| A | tonic | 4 | key tonic pitch class 0-11 |
| | mode | 1 | 0 major, 1 minor |
| | degree | 4 | root relative to the tonic 0-11; 14 solo line; 15 silence |
| | mask | 11 | bit i-1 set iff the interval i semitones above the root is present |
| | bass | 4 | bass pitch class relative to the root |
| | barlen | 6 | length of the token's bar in sixteenths, minus 1 |
| | reserved | 2 | zero |
| B | bar | 16 | bar from the start of the piece |
| | onset | 6 | sixteenths after the bar's downbeat |
| | dur | 8 | length in sixteenths |
| | cont | 1 | continues a chord split by the extractor |
| | reserved | 1 | zero, so every token is also a valid signed 64-bit integer |

## Hearing the chords: jenny and midimix

Two companion tools turn a token stream back into music. [jenny](https://github.com/twobob/jenny)
reads ChordStream's token table and writes the chords as a MIDI file, optionally arpeggiated;
[midimix](https://github.com/twobob/midimix) merges MIDI files into one, so the chords can be heard
against the original.

`tools\fetch_tools.bat` downloads the latest release of each into `tools\`. The executables are not
kept in this repository.

`test\octet_in_Dminor.mid` is a short example. Build `chordstream.exe` in the repository root, then
run from `tools`, so that the MIDI files it writes stay there:

```
cd tools
fetch_tools.bat
cmd /c "..\chordstream.exe --keep-add9 ..\test\octet_in_Dminor.mid | .\jenny.exe --rate 16 --arp randomonce -o chords.mid --vel 120 | .\midimix.exe -o randomonce16.mid ..\test\octet_in_Dminor.mid chords.mid"
```

```
jenny: 97 chords, 1043 notes -> chords.mid
midimix: 9 + 1 tracks -> randomonce16.mid
```

`tools\randomonce16.mid` is the octet with its extracted chords added as a sixteenth-note arpeggio in
random order. Keep the `.\` before `jenny.exe` and `midimix.exe`: some Windows configurations do not
search the current folder for programs.

## Licence

MIT License

Copyright (c) 2026 PsiPi

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial
portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT
LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
