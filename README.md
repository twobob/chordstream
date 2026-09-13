# ChordStream 0.1

ChordStream is a compact symbolic representation of harmony for training music models: one 64-bit
token per chord change or silence, carrying the key, the chord's degree relative to that key, its
pitch-class content, its bass, and its bar, onset, bar length and duration in musical time. It is
deterministic, invariant to tempo, and self-describing under windowing: any slice of tokens states its
own bar, key and bar length.

This repository holds the reference extractor, `chordstream_tokens.py`, and the paper that specifies the
format and its extraction in full, `chordstream.pdf`. The paper is written so that the representation
can be re-implemented from it alone.

## Requirements

Python 3.11 (the version it was tested on), with `numpy` and `mido`. `symusic` is used when installed and makes MIDI loading
much faster. Reading REMI token streams additionally needs `miditok`; the paper's results used
miditok 3.0.6 and symusic 0.6.0.

```
pip install numpy mido symusic miditok
```

## Use

From the command line, print the token stream of one or more MIDI files:

```
python chordstream_tokens.py song.mid
```

`python chordstream_tokens.py --help` lists every option, including reading documents from a REMI token
cache and cutting them into windows.

From Python:

```python
import chordstream_tokens as ct

tokens = ct.encode("song.mid")           # list of Token
packed = ct.to_array(tokens)             # numpy uint64, one value per token
for t in tokens[:4]:
    print(t.bar, t.onset, t.dur, t.label)

same = ct.unpack(int(packed[0]))         # a Token back from its 64 bits
```

For a REMI token stream, `ct.windows(ids, size, tokenizer=tok)` yields each window's covered time and
the chord tokens sounding in it, and `ct.encode_remi(ids, tok)` gives the stream for a whole document.

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

The paper defines every field, edge case and extraction step exactly.

## How well it works

Measured against human harmonic annotation with the evaluation protocol and test splits of BACHI:

| test set | root | quality | bass | full chord |
|---|---|---|---|---|
| POP909-CL, pop (91 pieces) | 86.0 | 70.4 | 91.2 | 67.3 |
| When in Rome and DCML, classical (150 pieces) | 60.9 | 52.5 | 61.4 | 39.1 |

On pop it is level with a published rule-based chord recogniser and better on bass; learned
recognisers are well ahead, particularly on classical music. The paper gives the comparison, the key
detection results and the known limitations.

## Version

0.1 is the first public release: the extractor and its specification. No model trained with
ChordStream is included or reported.

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
