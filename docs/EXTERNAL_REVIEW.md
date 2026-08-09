# External Review — Stage 6, Part A

## Method

A representative, not exhaustive, sample: public Kalshi trading bots, Kalshi/Polymarket
arbitrage bots, LOB-replay backtesting frameworks, open-source market-making engines,
trading-dashboard projects, and recent (2026) academic work on prediction-market
microstructure and cross-platform pricing. Sources are linked inline; GitHub projects
whose README could not be independently fetched and read (several Kalshi-bot search
results were near-duplicate, keyword-stuffed repos with no verifiable content — the
same "don't trust what you haven't checked" discipline this project's own CLAUDE.md
applies to Kalshi's wire schema) are named only where their actual README was read, not
just a search-snippet description of one. This document is Part A's own deliverable;
Part C (cross-market) and Part D (external information) get their own docs since the
plan scopes them as separate investigations, not sub-entries here.

Every capability below is judged against one question: does *this* project — an
integer-cent, allocation-free, single-writer-per-thread C++ execution engine whose whole
premise is measuring rather than assuming — actually need it, or is it a feature that
makes sense in a different kind of system. Several capabilities below are REJECTed for
exactly that reason, not because they're bad ideas in general.

---

## 1. Inventory-aware quoting (Avellaneda-Stoikov reservation price + skew)

- **Where it appears**: [rodlaf/KalshiMarketMaker](https://github.com/rodlaf/KalshiMarketMaker)
  (per-market workers computing "reservation price, asymmetric quotes, and order sizes"
  with inventory risk aversion that escalates near position limits);
  [fedecaccia/avellaneda-stoikov](https://github.com/fedecaccia/avellaneda-stoikov) and
  several other standalone implementations; the original Avellaneda & Stoikov (2008)
  optimal-market-making paper itself, still the reference model the field cites.
- **Problem it solves**: a market maker quoting a fixed spread around the current mid
  (this project's `PassiveMarketMaker`) has no notion that holding a growing position is
  itself a cost — it quotes the same width whether flat or already leaning heavily one
  way. Avellaneda-Stoikov skews the quote (a *reservation price* offset from mid,
  proportional to inventory × risk aversion × remaining time) so accumulating more of
  the same-side position gets progressively less attractive to the strategy itself, not
  just capped by an external risk check.
- **Do we have it**: No. `PassiveMarketMaker` (strategy/passive_market_maker.hpp) quotes
  a fixed `min_spread_`/`quote_qty_` and only reacts to an *existing* position by
  crossing to flatten it (`state.position != 0` → `Cross`) — reactive, not
  anticipatory. `RiskEngine::check`'s exposure cap is the only inventory-aware gate in
  the system, and it's a hard reject, not a quote adjustment.
- **Relevant**: Yes — this is the single most-cited gap between this project's simplest
  strategy and what a "real" market maker does.
- **Decision: DEFER.**
- **Reason**: the plan is explicit — "Do NOT immediately add random new strategies" and
  "Do NOT optimise the strategies for profitability yet. First establish unbiased
  measurements." Building inventory-skewed quoting *before* Part B's evaluation
  framework exists would be exactly the mistake the plan is structured to prevent:
  tuning a strategy against no measurement. Once Part H's comparison table exists, this
  becomes the natural, evidence-grounded next strategy to prototype — deferred to that
  point, not rejected.

## 2. Realistic backtesting: queue position, latency, partial fills

- **Where it appears**: [nkaz001/hftbacktest](https://github.com/nkaz001/hftbacktest)
  (Rust core with Python bindings; explicitly models "order queue position for order
  fill simulation," configurable feed/order latency, tick-by-tick L2/L3 reconstruction —
  read directly from its README, not a search snippet); the newly published
  **PredictionMarketBench** (Kalshi-specific — see arXiv:2602.00133, four real episodes
  from January 2026 spanning crypto/weather/sports, 8K–312K orderbook snapshots each),
  whose simulator implements maker-queue positioning and Kalshi's actual fee formula
  (see §3 below).
- **Problem it solves**: a naive backtest that assumes "my resting order fills whenever
  the book crosses its price" overstates every passive strategy's fill rate and
  understates its adverse selection — a resting order genuinely has to wait behind
  whatever quantity was already at that price level ahead of it.
- **Do we have it**: Partially. `tests/test_replay_determinism.cpp` and
  `workload/generator.hpp` already replay realistic wire-shaped events through the real
  `Ingress → Brain` path and assert deterministic `ShadowBook` state — but nothing
  downstream simulates *this account's own* fills, so there is currently no way to ask
  "if `PassiveMarketMaker` had been running against this data, what would it have made."
  See Part B.2's own assessment doc section for the full gap analysis.
- **Relevant**: Yes — directly the plan's central question for Stage 6 ("does the system
  have a meaningful way of evaluating whether its execution decisions are actually
  good").
- **Decision: BUILD.**
- **Reason**: this is Part B.3 of this stage. Scoped deliberately smaller than
  `hftbacktest` or `PredictionMarketBench` — this project doesn't need multi-exchange
  L3 reconstruction or a Numba JIT strategy runtime, it needs queue-position-aware fills
  for the three strategies it already has, reusing `ShadowBook`/`Strategy` as-is. See
  `docs/BACKTEST_ASSESSMENT.md`.

## 3. Fee-aware P&L (maker/taker differentiated)

- **Where it appears**: Kalshi's own published
  [fee schedule](https://kalshi.com/docs/kalshi-fee-schedule.pdf) and the identical
  formula independently confirmed in Kalshi's CFTC DCM rule filing
  ([rule091222kexdcm003.pdf](https://www.cftc.gov/sites/default/files/filings/orgrules/22/09/rule091222kexdcm003.pdf)):
  **taker fee = round_up(0.07 × C × P × (1−P))**, **maker fee = round_up(0.0175 × C × P
  × (1−P))**, where `P` is price in dollars and `C` is contract count — a 4× maker
  discount, peaking at a 50¢ price and decaying toward both tails. PredictionMarketBench
  implements this exact formula.
- **Problem it solves**: without it, "gross P&L" and "net P&L" are the same number,
  which silently overstates every strategy's real profitability — worse for
  taker-heavy strategies (`TradeFlow` crosses; `PassiveMarketMaker`'s exit crosses too)
  than maker-heavy ones.
- **Do we have it**: No. Nothing in this codebase currently models Kalshi's trading
  fees at all.
- **Relevant**: Yes — explicitly requested by Part B.3's metric list (`fees`,
  `net P&L`).
- **Decision: BUILD.**
- **Reason**: the formula is externally verified (two independent sources — Kalshi's
  own PDF and its regulatory filing — agree exactly), integer-cent-compatible (rounds up
  to a centicent, the same discipline this project already applies to prices), and small
  enough to implement as one pure function. See `eval/fees.hpp`.

## 4. Cross-exchange arbitrage (Kalshi ↔ Polymarket)

- **Where it appears**: numerous public bots (`ImMike/polymarket-arbitrage`,
  `TopTrenDev/polymarket-kalshi-arbitrage-bot` in Rust, and others of widely varying
  code quality — several search results for this category were unverifiable
  keyword-stuffed repos, treated as noise, not evidence); academically,
  Gebele & Matthes, *"Semantic Non-Fungibility and Violations of the Law of One Price in
  Prediction Markets"* (arXiv:2601.01706) is the rigorous version of the same question.
- **Problem it solves**: the same real-world event, listed on two independent venues
  with separate liquidity pools, can price it differently — a structural inefficiency
  a bot can try to capture.
- **Do we have it**: No.
- **Relevant**: investigated on its own merits in Part C, not decided here — see
  `docs/CROSS_MARKET_RESEARCH.md`.
- **Decision: see Part C.**
- **Reason**: the plan explicitly separates this investigation out; folding a
  BUILD/DEFER/REJECT call into this table without doing the actual investigation would
  be exactly the "blindly copy features" mistake the plan warns against.

## 5. LLM/AI-ensemble signal generation

- **Where it appears**: `bestpracticaI/kalshi-ai-trading-bot` (OpenRouter LLM
  integration), `0mnjb/Kalshi-AI-Trading-Bot` ("five-model AI ensemble") — both real
  repos, evaluated only on their stated architecture, not run.
- **Problem it solves**: attempts to turn unstructured text (news, sentiment) into a
  trading signal without hand-written parsing logic.
- **Do we have it**: No, and this project's `OracleFeed` interface
  (`strategy/oracle_feed.hpp`) is deliberately structured/typed, not text-based.
- **Relevant**: No, by the plan's own explicit instruction.
- **Decision: REJECT.**
- **Reason**: the plan states outright — "Do NOT add an LLM to the trading hot path,"
  "Do NOT immediately add random new strategies or AI/ML features." Beyond just
  following the instruction, it cuts against this project's own stated ethos: every
  number here is measured and every decision traceable (see the Strategy panel's
  explicit "explainable" goal in Part E) — an LLM-driven signal is neither.

## 6. Global (cross-market) portfolio exposure caps

- **Where it appears**: `rodlaf/KalshiMarketMaker`'s `max_global_contracts` /
  `max_contracts_per_market` two-tier cap.
- **Problem it solves**: `RiskEngine::check` (risk/risk_engine.hpp) caps notional
  exposure *per market* only (`position_[market]`) — nothing sums exposure across all
  `kMaxMarkets` (1024) markets a single account might be trading simultaneously, so a
  strategy registered on many markets at once could accumulate large aggregate exposure
  that no single check ever sees.
- **Do we have it**: No — a real, confirmed gap, not a hypothetical one.
- **Relevant**: Yes.
- **Decision: DEFER.**
- **Reason**: real, but orthogonal to this stage's actual charter (evaluation +
  observability, not a risk-engine redesign). `RiskEngine`'s own header comment already
  states its scope discipline ("exactly three things and nothing more") deliberately;
  extending it belongs to a future pass with its own tests, not a paragraph bolted onto
  Stage 6. Flagged here so it isn't silently lost.

## 7. Sharpe ratio / max-drawdown / equity-curve strategy scorecards

- **Where it appears**: PredictionMarketBench (Sharpe, max drawdown, fill ratio, equity
  curve as standardized outputs); `Viprasol-Tech/kalshi-trading-bot`'s backtester
  (Sharpe, max drawdown, volatility, win rate, profit factor, fees, per-trade P&L —
  though built on "offline synthetic data" via `random_walk_snapshots()` with, in its
  own words, "simplified fills with no queue, slippage, or settlement modelling," and it
  says so explicitly rather than overclaiming realism).
- **Problem it solves**: turns raw fills into the standard vocabulary needed to compare
  strategies against each other, not just against zero.
- **Do we have it**: No.
- **Relevant**: Yes — this is the literal content of Part B.3's metric list.
- **Decision: BUILD.**
- **Reason**: part of the same evaluation framework as #2/#3 above.

## 8. Real-time observability dashboard (positions, P&L, latency, connection health)

- **Where it appears**: [OpenAlgo](https://github.com/marketcalls/openalgo) (WebSocket
  proxy + PnL tracker + latency monitoring); Deephaven's trading-dashboard tutorials
  (real-time P&L/VaR tables); `rodlaf/KalshiMarketMaker`'s own `kalshi-dashboard` tool
  (account money/positions/resting-orders view); `Viprasol-Tech/kalshi-trading-bot`
  lists a web dashboard only on its roadmap, not built yet, and says so.
- **Problem it solves**: exactly what Part E of this stage asks for — a fast answer to
  "is the system working, what is it doing, is it making money" without reading raw
  logs.
- **Do we have it**: No.
- **Relevant**: Yes.
- **Decision: BUILD.**
- **Reason**: Part E/F of this stage. See `docs/DASHBOARD.md` and the `telemetry/` /
  `dashboard/` implementation for the specific read-only, non-blocking design this
  project's architecture requires (unlike the systems above, `Brain` here is a single
  pinned thread with no lock — a dashboard that could ever stall it is a correctness
  bug, not just a UX one).

## 9. Realized-spread / adverse-selection measurement

- **Where it appears**: standard market-microstructure literature (Huang & Stoll-style
  spread decomposition: effective spread = order-processing/inventory component +
  adverse-selection residual, the latter recovered by comparing the fill price to the
  midpoint some seconds/minutes later); more elaborate order-flow-toxicity models like
  VPIN (Easley/López de Prado/O'Hara) exist in the wild (a "VPIN toxicity detection"
  claim appears in search results for a project called "Atomic Mesh," which could not be
  independently verified — its GitHub repository was not locatable via search, so it is
  named here only as a claim seen, not as a checked fact, the same distinction this
  project's own README draws between verified and unverified claims).
- **Problem it solves**: distinguishes a strategy that's genuinely capturing spread from
  one that's getting picked off — its resting fills are systematically followed by the
  market moving against it.
- **Do we have it**: No.
- **Relevant**: Yes — explicitly on Part B.3's "where possible" metric list
  (`adverse selection`, `realised spread`).
- **Decision: BUILD** (the standard realized-spread decomposition), **REJECT** (a full
  VPIN-style toxicity classifier).
- **Reason**: the realized-spread/adverse-selection split is a direct, well-established
  computation over data this project's fill simulator already produces — proportionate
  to what Part B.3 actually asks for. VPIN is a heavier statistical model (volume-time
  bucketing, order-flow classification) that would be new machinery in search of a
  question nobody asked; disproportionate complexity for what this metric needs to
  answer.

## 10. On-chain trade-direction ground truth (Polymarket-specific data-quality gap)

- **Where it appears**: *"The Anatomy of a Decentralized Prediction Market:
  Microstructure Evidence from the Polymarket Order Book"* (arXiv:2604.24366) — finds
  that inferring trade direction from Polymarket's public feed alone (Lee-Ready-style
  inference) is only ~59% accurate versus on-chain `OrderFilled` ground truth, well
  below the ~80% Lee-Ready accuracy typical on Nasdaq, because Polymarket settles
  through an AMM/CLOB hybrid on-chain rather than a centralized matching engine.
- **Problem it solves**: a real, carefully-measured data-quality problem for anyone
  building Polymarket microstructure research.
- **Do we have it**: N/A.
- **Relevant**: **No** — this project only ever connects to Kalshi, a centralized
  exchange whose `trade` WS channel already tags `taker_book_side` directly (see
  `live/kalshi_feed.cpp`); there is no trade-direction inference problem here to
  inherit.
- **Decision: REJECT** (as inapplicable).
- **Reason**: named explicitly as an example of the plan's own instruction — "do not
  blindly copy features from other projects." A capability that's a real, hard-won
  finding in one system's domain (decentralized, on-chain order flow) can be simply
  irrelevant in another's (centralized, already-tagged trade feed); citing it without
  checking whether it transfers would be exactly the mistake to avoid.

## 11. Rust-for-periphery / C++-for-hot-path hybrid architecture

- **Where it appears**: a project self-described in search results as "Atomic Mesh" —
  "Rust + C++17 FFI... C++ for the hot path... Rust for the rest of the system," "live
  on Binance Testnet." Its specific GitHub repository could not be located and
  independently read, so — same discipline as item 9 — this is named as a directional
  pattern seen in the wild, not a verified citation.
- **Problem it solves**: gives a memory-safe, batteries-included web/async ecosystem to
  the parts of a trading system that aren't latency-critical (dashboards, tooling)
  without touching the hot path's language or ABI.
- **Do we have it**: this is exactly this stage's own dashboard architecture — Rust
  (Axum) for the read-only observability web server, C++ untouched everywhere else. Not
  copied from this specific unverifiable source; arrived at independently from this
  project's own constraint (per this conversation's instructions, the dashboard uses
  Rust) plus the existing hard boundary CLAUDE.md already draws between "Trading State"
  and "Telemetry/Presentation State."
- **Relevant**: Yes.
- **Decision: BUILD** — already this stage's plan; noted here only because the pattern
  independently shows up elsewhere too.

## 12. Pluggable strategy framework with example strategies

- **Where it appears**: `Viprasol-Tech/kalshi-trading-bot` (subclass one `Strategy`
  base, five bundled examples: market maker, momentum, mean-reversion, arbitrage,
  Kelly-sized fair-value).
- **Problem it solves**: lets a user swap strategies without touching engine code.
- **Do we have it**: Yes, already — `strategy::Strategy` plus `PassiveMarketMaker`,
  `OrderBookImbalance`, `TradeFlow`, `OracleThresholdStrategy` (Stage 3).
- **Relevant**: N/A (already have it).
- **Decision: KEEP as-is.**
- **Reason**: the plan explicitly says not to add new strategies this stage. No action.

## 13. Official Kalshi historical data endpoints

- **Where it appears**: Kalshi's own documented `/historical/markets`,
  `/historical/markets/{ticker}/candlesticks`, `/historical/trades`, `/historical/fills`,
  `/historical/orders`, `/historical/positions` REST endpoints
  (docs.kalshi.com/getting_started/historical_data — fetched and read directly, not
  assumed) — separate from several third-party paid resellers found in the same search
  (Lychee Data, Predexon, kalshibacktest.com) offering repackaged/scraped Kalshi data.
- **Problem it solves**: would let Part H's evaluation run against real settled markets
  instead of only synthetic seeded replay.
- **Do we have it**: No integration exists.
- **Relevant**: Yes, as a future upgrade to Part B.3's evaluation framework.
- **Decision: DEFER.**
- **Reason**: real and valuable, but pulling and validating a real historical dataset is
  its own multi-hour-plus data-engineering effort, separate from what's tractable
  alongside building the framework itself this stage. `eval/` (Part B.3) is deliberately
  built against an abstract event stream (the same `BookEvent`/`ExecReport` shapes the
  live pipeline already uses) specifically so a real historical-data replay source can
  be plugged in later without redesigning the evaluator. Third-party resellers are
  rejected outright as a data source on this project's own standing principle: only
  Kalshi's own docs/endpoints are treated as ground truth (see CLAUDE.md's "every
  external API fact is independently verified" rule) — a scraped repackaging is exactly
  the kind of unverified secondhand fact that rule exists to keep out.

---

## Summary table

| # | Capability | Decision |
|---|---|---|
| 1 | Inventory-aware (Avellaneda-Stoikov) quoting | DEFER |
| 2 | Realistic backtesting (queue/latency/partial fills) | **BUILD** |
| 3 | Maker/taker fee-aware P&L | **BUILD** |
| 4 | Cross-exchange arbitrage | see Part C |
| 5 | LLM/AI-ensemble signals | REJECT |
| 6 | Global cross-market exposure caps | DEFER |
| 7 | Sharpe/drawdown/equity-curve scorecards | **BUILD** |
| 8 | Real-time observability dashboard | **BUILD** |
| 9 | Realized-spread / adverse-selection metric | **BUILD** (simple form only) |
| 10 | On-chain trade-direction inference | REJECT (inapplicable) |
| 11 | Rust-periphery / C++-hot-path split | **BUILD** (this stage's dashboard) |
| 12 | Pluggable strategy framework | KEEP (already have) |
| 13 | Official Kalshi historical data endpoints | DEFER |

Items 2, 3, 7, 9 together are Part B.3's evaluation framework
(`docs/BACKTEST_ASSESSMENT.md`, `eval/`). Item 8/11 is Part E/F/G
(`telemetry/`, `dashboard/`). Item 4 is resolved on its own in
`docs/CROSS_MARKET_RESEARCH.md`. Items 1, 6, 13 are logged, real, and
deliberately not acted on this stage — see `docs/DECISION_GATE.md` for the
final KEEP/BUILD FURTHER/DEFER/REJECT roll-up across every part of this
stage, not just this one.
