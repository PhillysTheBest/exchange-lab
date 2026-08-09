# External Information Investigation — Stage 6, Part D

## What this is investigating

Whether an independent signal — weather, economic releases, news, official
announcements, other structured datasets — can improve on this project's existing
microstructure-only strategies, and whether it can be wired in without violating the
plan's hard constraint: **no LLM in the trading hot path**, and the execution system
stays low-latency and deterministic even if signal generation itself is slow.

## The architecture this asks for already exists

The plan's target shape:

```
External Data -> Independent Signal -> MarketState/Feature -> Strategy -> ExecutionDecision
```

This project already built almost exactly this, in Stage 1/Phase 4, for a different
reason (turning a real-world event into a trading decision without any book/oracle
coupling): `strategy/oracle_feed.hpp`'s `OracleFeed` interface — a pluggable source
running on its own thread, publishing into a lock-free ring, decoupled from `Brain`'s
own book-driven dispatch — plus `TriggerKind::OracleEvent` and
`OracleThresholdStrategy` (`strategy/oracle_threshold_strategy.hpp`) as a working,
tested, end-to-end demonstration of "external event in, `ExecutionDecision` out"
(`tools/strategy_demo.cpp`, verified arithmetic: 10 sells × 5 @ 8¢ + 1 buy × 5 @ 92¢
nets exactly −60¢ cash / −45 position). The README's own Phase 4 section names NOAA,
SEC EDGAR, and the FAA as exactly the kind of source this was built for, and is
explicit that **only synthetic/simulated feeds ship** — "nothing here calls out to
NOAA, EDGAR, or the FAA" — a deliberate scope boundary from Stage 1, not a Stage 6 gap.

So the real question Part D has to answer is narrower than "design the architecture" —
it already exists. The question is: for each candidate source, is a *real* feed worth
building, given what it would actually cost to wire in and what it could actually
deliver.

## Candidate sources, checked (not assumed)

### Weather — NOAA/National Weather Service API

- **What it is**: `api.weather.gov`, the official NWS API. No API key required, free
  for any use subject to reasonable rate limits, RESTful JSON (GeoJSON for spatial
  data), real-time forecasts/observations/alerts.
- **Fit**: this project's own `tools/strategy_demo.cpp` already simulates exactly this
  category — a synthetic feed "hovering near a 90°F threshold" — and Kalshi lists real,
  currently-active daily-high-temperature markets by city (`KXHIGHNY` appears in this
  project's own README examples and PERFORMANCE.md). This is the single best-fit
  candidate of everything investigated: structured numeric data, a free real-time
  official API, and a Kalshi market category that settles directly on it. No NLP, no
  LLM, no ambiguity about what the number means.
- **What it would actually take**: implement `OracleFeed::run` against
  `api.weather.gov`'s forecast/observation endpoints for the specific stations Kalshi's
  temperature markets settle against, publish `OracleEvent`s the same shape
  `strategy_demo`'s synthetic feed already produces. The consuming side
  (`OracleThresholdStrategy` or a purpose-built successor) needs no new architecture.
- **Decision: DEFER**, not BUILD. It's real, well-scoped, and the best candidate here —
  but it is a genuinely new feature (a live network integration with its own failure
  modes: NWS API downtime, station data gaps, forecast-vs-settlement-rule mismatches
  that would need their own verification pass) and the plan is explicit that Stage 6 is
  not the stage for adding new capabilities of this kind ("Do NOT immediately add
  random new strategies or AI/ML features" — a live weather feed isn't AI/ML, but it is
  exactly the kind of new-capability scope-creep that instruction is guarding against
  ahead of Part B/H's evaluation work actually existing). Recommended as the first
  candidate to build in a future stage, specifically because this investigation found a
  concrete, low-friction, well-matched real API for it — not a vague "weather could be
  useful" gesture.

### Economic releases — BLS Public Data API

- **What it is**: the Bureau of Labor Statistics' public API (no registration
  required) for CPI, employment, and other headline releases. Checked directly: **the
  API itself carries a one-day lag between a release and its availability via the
  API** — confirmed from BLS's own API documentation, not assumed. Release *timing*
  itself is well-documented and highly regular (CPI/NFP/GDP/PCE conventionally 08:30
  ET).
- **Fit**: poor, for the obvious reason. A trading signal that only has the actual CPI
  number a day after the market has already priced the live release is worthless as a
  price signal — the entire tradeable event (the market's reaction) has already
  happened. The *release calendar* (knowing 08:30 ET on a specific date is a scheduled
  volatility event, without knowing the number) is a genuinely different, much smaller
  claim: not a directional signal, but a known-risk-window flag a strategy or the risk
  engine could use to widen spreads or pause quoting around a scheduled release —
  closer to a calendar lookup table than a live feed.
- **Decision: REJECT** the API itself as a trading-signal source (confirmed unusable
  for that purpose by its own documented lag — not a judgment call). **DEFER** the much
  narrower "known scheduled-release risk window" idea — real, cheap (a static calendar,
  not a live integration), but it's a risk-engine feature, not a Strategy feature, and
  belongs with item 6 in `docs/EXTERNAL_REVIEW.md` (global exposure caps) as a future
  `RiskEngine` extension, not something Part D should build standalone.

### News / official announcements

- **What it is**: the category the plan explicitly calls out as needing independent
  investigation, and the category where every real-world "AI trading bot" found in
  Part A (`bestpracticaI/kalshi-ai-trading-bot`, `0mnjb/Kalshi-AI-Trading-Bot`) reaches
  for an LLM — because turning unstructured natural-language text into a structured
  trading signal is, definitionally, an NLP problem.
- **Fit**: this is exactly where the plan's hard line sits — "Do NOT add an LLM to the
  trading hot path." Note this project's own `OracleFeed` design already keeps signal
  generation off the hot path (it runs on its own thread, publishing into a ring — see
  the file header's "may be slow" allowance, which the plan's Part D repeats
  verbatim), so an LLM summarizer *off* the hot path, in a separate process publishing
  structured `OracleEvent`s, would not technically violate that specific rule. It's
  rejected anyway, on two independent grounds: (1) the plan separately says "Do NOT
  immediately add random new... AI/ML features" — a blanket instruction, not qualified
  by "unless it's off the hot path" — and (2) this project's own stated ethos is
  measured, explainable decisions (see the dashboard's Strategy panel goal, Part E) —
  an LLM-derived signal is opaque in exactly the way every other signal in this system
  deliberately isn't.
- **Decision: REJECT.**

### Official announcements via structured feeds (SEC EDGAR, FAA, etc.)

- **What it is**: the other two sources `strategy/oracle_feed.hpp`'s own header
  comment already names as hypothetical fits. Both offer structured (XML/JSON/CSV),
  non-NLP feeds for specific event types (EDGAR full-text search / filing index; FAA
  NOTAMs/flight data).
- **Fit**: plausible in principle, but neither maps to an existing Kalshi market
  category this project has any evidence about (unlike weather, where the settlement
  mechanism is a direct number match). Investigating either further without a
  concrete Kalshi market to point it at would be speculative scope, not evidence-based
  investigation.
- **Decision: DEFER**, unexamined beyond noting they remain the same "structurally
  fine, not evidenced yet" category as weather but with no concrete market lined up —
  lower priority than the weather candidate specifically because that gap is missing.

## Decision: DEFER (design already exists; no new feed built this stage)

No independent signal is wired into the live system this stage. This is Outcome D
territory, same as Part C: "None [provide convincing evidence]... That is still a
successful result" — except here the honest finding isn't "investigated and found no
evidence," it's "found one well-matched, verified, buildable candidate (NOAA/weather)
and explicitly chose not to build it yet," because building it now would be exactly
the kind of pre-evaluation feature addition the plan structures this whole stage to
avoid. The interface that would carry it (`OracleFeed` → `OracleEvent` →
`Strategy::on_event(..., TriggerKind::OracleEvent, ...)`) needs no new design work —
it's Stage 1's, already tested, already wired through `Brain::evaluate_oracle_event`.
