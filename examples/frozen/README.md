# A frozen model

A worked example of the one way a learned model may take part in a session: trained
elsewhere, its weights rounded to integers and written into a C++ header, and then run like
any other strategy. [`docs/CODING-AGENT.md`](../../docs/CODING-AGENT.md) says why a model may
never be *called* from inside a session. This example shows what is allowed instead.

| File | What it is |
|---|---|
| `train.py` | the training script: standard-library Python, reads a tape, fits, writes the header |
| [`../strategies/frozen.hpp`](../strategies/frozen.hpp) | what it wrote: the strategy `frozen`, generated, never edited by hand |

## Make it again

```bash
build/release/exchange-lab -c '/calibrate --known=known'     # writes data/known.tape
python3 examples/frozen/train.py --check data/known.tape     # identical, byte for byte
python3 examples/frozen/train.py data/known.tape > examples/strategies/frozen.hpp
```

The first line is exercise 9's (`docs/LEARN.md`); the app builds `calibrate` if it is
missing. `--check` regenerates the header and compares it with the committed one.

## What was trained, on what

- **The data** is the known-answer control's tape ([`docs/DATA.md`](../../docs/DATA.md)): eight
  sessions of the same market model with other parameters (half spread 3 ticks, `noise.qty`
  40, `noise.timer_ns` 20000), on seeds 7001 to 7008. No block a strategy is scored on uses those
  seeds, so the model is never trained on the seeds it is judged on. The tape is the same on
  every machine, and `make canonical` pins its checksum (`tape.known 1284219359 927453`).
  The header records that checksum and the training script's.
- **The model** is linear: how far the mid moves over the next 5 prints, in half ticks, from
  the touch imbalance, where the last print was against the mid, and the spread. It is fitted by
  least squares on 14,601 touch changes and explains little: R² 0.136 on the data it was
  fitted to, which says nothing about any other data.
- **The fitting** uses + − × ÷ only (Gaussian elimination), which IEEE 754 rounds exactly, so
  running the script again gives the same weights on any machine. A model needing `exp()` or
  `log()` would be reproducible only as far as the maths library is, so the frozen header,
  not the training run, would be the record.
- **The freeze** multiplies each weight by 2^16 and rounds it: `kBias 58670`, `kImbalance 91`,
  `kLastPrint 3811`, `kSpread -13615`. The strategy computes the same features in the same
  integers (`train.py` truncates its divisions toward zero, as C++ does) and adds them up.

## How it trades, and what it did

Its predictions are smaller than the half spread it would pay to cross (at a 3-tick spread
the largest is under two thirds of it), so taking liquidity on them could never pay. It rests one order at the touch instead, on the side the model favours, and pulls
it when the prediction turns or the touch moves away. On the standard block
(`suite_lab --candidates=zi,imbalance,frozen`):

| arm vs baseline | mean ± s.e. | verdict |
|---|---|---|
| frozen vs placebo | +5.31 ± 3.38 | not separated |
| frozen vs zi | +97.13 ± 5.11 | separated |
| imbalance vs placebo | −34.92 ± 3.09 | separated |

The model was trained on a different market from the one it is scored on, and the result is
**not established**: the interval runs from −1.44 to +12.06, and +5.31 is under the ±9.45 that
64 seeds can resolve. It does beat chance (`zi`) and the strategy it borrowed its first
feature from (`imbalance`). Report it that way. The run records `frozen.hpp`'s cksum
(`run.candidate.frozen.digest`), so `/reproduce` refuses the run once the header changes:
the model a result came from is the model in that file.

## Your own

A model of your own follows the same path: fit it anywhere, with anything, and write integer
weights into a header that follows `examples/strategies/imbalance.hpp`'s convention. Keep its
features to what a `MarketView` holds, compute them in the same integers in training and in
the header, train on seeds or tapes no block you report on uses, and record the training
data's checksum in the header. Then tune on `standard` and confirm once on `holdout`
([`docs/LEARN.md`](../../docs/LEARN.md), exercise 10).
