# Cross-Market Research — Stage 6, Part C

## The question, verbatim from the plan

> "When another market moves, does Kalshi subsequently move?"

This document answers it in two layers: (1) what the existing, properly-powered
peer-reviewed literature already found, and (2) one real, live, honestly-reported
pilot run of this project's own offline research tool
([`research/cross_market/kalshi_polymarket_snapshot.py`](../research/cross_market/kalshi_polymarket_snapshot.py)),
kept — as the plan requires — entirely outside `include/`, `pipeline/`, `strategy/`,
`live/`: it is a standalone Python script with no build-system integration, no
dependency from any C++ translation unit, and no path by which it could ever run on or
near the trading hot path.

## What the literature already found (properly powered; this pilot is not)

Gebele & Matthes, *"Semantic Non-Fungibility and Violations of the Law of One Price in
Prediction Markets"* (arXiv:2601.01706), built a semantic-alignment framework to
identify genuinely-equivalent contracts across ten major prediction-market venues
(2018–2025) and found:

- ~6% of all events are concurrently listed across platforms.
- For those matched pairs, **persistent execution-aware price deviations of 2–4% on
  average**, even in liquid, information-rich markets.
- The paper attributes this to **structural frictions** (differing fee schedules,
  collateral/redemption mechanics, contract design — e.g. fixed-strike vs
  forward-start) **rather than informational disagreement**.

That last point directly bears on the plan's question: a paper built specifically to
answer "how much do these markets disagree" attributes the disagreement to *why the two
venues are different products*, not to one venue being informationally behind the
other and later catching up. It presents no lead-lag/Granger-style result at all — the
paper doesn't claim one, and this document doesn't invent one on its behalf.

## A real pilot run, today

`kalshi_polymarket_snapshot.py` pulls **live, public, unauthenticated** data from both
venues — no scraping, no third-party reseller, the same "only the exchange's own
documented endpoints count as ground truth" discipline CLAUDE.md already holds for
Kalshi's WS/REST schema (see `live/kalshi_feed.hpp`/`live/kalshi_rest.hpp`'s SCHEMA
STATUS notes). Both endpoints were fetched and read directly, not assumed:

- Kalshi: `GET https://external-api.kalshi.com/trade-api/v2/markets` — confirmed
  against `docs.kalshi.com/getting_started/api_environments`.
- Polymarket: `GET https://gamma-api.polymarket.com/public-search` — confirmed by
  calling it and reading the real response shape (including one real, undocumented
  quirk found this way: `outcomePrices` comes back as a **JSON-encoded string**, not a
  native array — handled explicitly in the script, not silently mis-parsed).

Both platforms currently list the same real event — the Fed's September 16, 2026
FOMC rate decision — Kalshi as a strike ladder (`KXFED-26SEP`, one binary market per
rate threshold), Polymarket as one event with five independent yes/no sub-markets. A
real run of the tool (2026-08-09, captured verbatim):

```
== Kalshi: KXFED-26SEP ==
  KXFED-26SEP-T3.75    mid=0.3450  bid=0.3400 ask=0.3500  ...above 3.75%...
  KXFED-26SEP-T3.50    mid=0.9900  bid=0.9800 ask=1.0000  ...above 3.50%...
  KXFED-26SEP-T4.00    mid=0.0050  bid=0.0000 ask=0.0100  ...above 4.00%...
  (full ladder, 11 strikes total, in research/cross_market/ — every other strike's
   mid matches its nearest neighbor above/below, i.e. the tails are flat)

== Polymarket: event matching 'fed-decision-in-september' ==
  p=0.6250  liq=$734,787   vol=$5,028,216  no change
  p=0.3550  liq=$562,210   vol=$4,280,158  increase 25 bps
  p=0.0175  liq=$731,734   vol=$5,590,452  decrease 25 bps
  p=0.0095  liq=$369,305   vol=$2,482,340  decrease 50+ bps
  p=0.0055  liq=$385,245   vol=$2,855,403  increase 50+ bps
```

The Fed's own target range going into this meeting is confirmed as 3.50%–3.75%
(upper bound 3.75%) by the Federal Reserve's own June 2026 FOMC statement
([federalreserve.gov](https://www.federalreserve.gov/newsevents/pressreleases/monetary20260617a.htm)) —
checked directly, not inferred solely from Kalshi's own ladder, though the ladder's
own shape (near-certain below 3.75%, a real split at 3.75%, near-zero above 4.00%)
independently corroborates it. That lets Kalshi's strike ladder be converted into the
same three-way outcome space Polymarket already uses:

| Outcome (Sept 2026 FOMC) | Kalshi (implied from ladder) | Polymarket | Abs. gap |
|---|---:|---:|---:|
| No change (stays 3.75%) | 64.5% | 62.5% | 2.0 pp |
| +25 bp hike (→4.00%) | 34.0% | 35.5% | 1.5 pp |
| +50 bp+ hike | 0.5% | 0.55% | 0.05 pp |
| Any cut | 1.0% | 2.7% | 1.7 pp |

(Kalshi's "no change"/"+25bp" figures are derived: `P(>3.50%) − P(>3.75%)` and
`P(>3.75%) − P(>4.00%)` respectively, since Kalshi's ladder is cumulative-style
"above X" strikes rather than Polymarket's mutually-exclusive buckets.)

## Reading this honestly

**What this pilot shows**: for the one real, currently-liquid, correctly-matched event
checked, the two platforms agree closely — every gap is 2 percentage points or
smaller. That's consistent with the literature's "2–4% average" characterization, not
a contradiction of it, and nowhere near the size of gap that would suggest one venue is
lagging the other's information.

**What this pilot does NOT show, and does not claim to**: a single snapshot cannot
demonstrate temporal lead-lag by construction — there is only one point in time here,
not a paired time series. Properly answering "does Kalshi subsequently move" needs
repeated paired snapshots (or, better, both venues' real trade tapes) across many
independent events, exactly the multi-year, 100,000-event panel the cited paper
already built and this session did not attempt to replicate. Collecting that panel is
a real, multi-hour-plus data-engineering project on its own (semantically matching
events across platforms by natural-language description is, per that paper, hard
enough to need its own dedicated framework) — out of proportion to what a research
side-investigation should cost inside a stage whose main deliverables are the
evaluation framework and the dashboard.

**One more honest cross-check**: Kalshi's own taker fee at a 50¢ price is
`round_up(0.07 × 0.5 × 0.5)` ≈ 1.75¢ per contract (see
`docs/EXTERNAL_REVIEW.md` item 3) — meaning even the largest gap observed here (2.0pp,
on "no change") is on the same order as the round-trip transaction cost of acting on
it on one leg alone, before even accounting for Polymarket's own fee/gas/slippage
structure on the other leg. A 2pp gap that's roughly fee-sized is a very different
finding from a 20pp gap that would be obviously exploitable regardless of costs.

## Decision: DEFER

Per the plan's own Outcome D framing ("None provide convincing evidence. That is still
a successful result"): **no evidence of exploitable, temporally-predictable divergence
was found or is claimed.** The existing literature's finding — structural, modest,
fee-sized gaps, not informational lag — is consistent with this pilot's own result, so
`ExternalMarketFeed → CrossMarketFeature → Strategy` is **not built**. The plan's own
conditional trigger for that interface — "If there is evidence of predictive value" —
is not met.

What ships instead: the real, working, reusable single-snapshot tool
(`research/cross_market/kalshi_polymarket_snapshot.py`), parameterized (not hardcoded
to the Fed pairing) so a future session with more time budget can extend it into an
actual scheduled panel-collector without redesigning anything, plus this document as
the honest record of what was and wasn't checked. Building the live
`ExternalMarketFeed`/`CrossMarketFeature` interface *now*, on this evidence, would be
solving a problem this investigation didn't find.
