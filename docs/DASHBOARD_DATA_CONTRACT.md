# Dashboard Data Contract — Stage 6A

Every field the redesigned dashboard (`dashboard/static/index.html`) renders, where
it actually comes from, and whether it's authoritative. Written per the redesign
plan's own Part 29 requirement ("before changing the UI, inspect the existing
backend and document exactly what information is currently available... if the
desired dashboard contains information that does not exist, identify it explicitly.
Do not fabricate it.") — this covers both what Part G (Stage 6) already published
and what this pass (Stage 6A) added to make the new layout possible without
inventing anything.

**Everything below traces to a real `telemetry::TelemetryState` field, itself
written only from `pipeline::Brain`'s own thread (T2) or, for the handful of
once-at-startup config values, `tools/kalshi_pipeline.cpp`'s main thread before T2
starts — see `telemetry/telemetry.hpp`'s own file header for the two-tier
writer discipline this all follows.** The frontend performs zero independent P&L
or risk-figure computation of its own — every number is either shown as published
or derived via a documented, stated identity (see "Derived, not fabricated" below),
never recomputed from a different formula than the backend's own.

## System status

| Frontend field | JSON path | Type | Update source | Authoritative? |
|---|---|---|---|---|
| Connection | `conn_status` | string | `Brain`'s `on_reconnecting`/book-event heuristic (`tools/kalshi_pipeline.cpp`) | Yes, but a heuristic — see that file's own comment: Ingress has no dedicated "connected" callback |
| Mode | `mode` | string | Set once at startup from `--demo` flag | Yes |
| Active markets | `active_markets` | number | Set once at startup from ticker count | Yes |
| Reconnects | `reconnect_count` | number | `Ingress::on_reconnecting` | Yes |
| Uptime | *(derived)* | — | `Date.now() - start_ts_ms` | Derived client-side from an authoritative field, not fabricated |
| Current time | *(derived)* | — | Browser's own clock | Not backend data at all — a clock display |

## Performance (hero)

| Frontend field | JSON path | Type | Update source | Authoritative? |
|---|---|---|---|---|
| Net P&L | `pnl_net_cents` | cents | `TelemetryState::net_pnl_cents()` — O(1), incrementally maintained (see below) | Yes |
| Realized | `pnl_realized_cents` | cents | Sum of each market's `PositionAccount::realized_pnl_cents()` | Yes |
| Unrealized | `pnl_unrealized_cents` | cents | Sum of each market's `PositionAccount::unrealized_pnl_cents(mid)` | Yes |
| Fees | `fees_cents` | cents | Sum of `fee_cents` passed to `on_order_filled` | **Partial** — see "Known gap: live fee tracking" below |
| P&L chart (session) | `pnl_history[]` | `{ts_ms, net_pnl_cents}[]` | **New this pass.** Sampled on every fill (unthrottled) + book updates (throttled to 1/2s) | Yes — real trajectory, not interpolated/smoothed |

`total_realized_pnl_cents_`/`total_unrealized_pnl_cents_` (the two running totals
`net_pnl_cents()` is built from) are maintained **incrementally** — each market's own
delta is `fetch_add`-ed in whenever `publish_account`/`on_book_update` changes it —
specifically so `on_order_filled`/`on_book_update` can sample a P&L history point in
O(1) from T2's own thread without ever scanning all `kMaxTelemetryMarkets` markets on
a per-event path. `telemetry_publisher.hpp`'s per-snapshot `pnl_realized_cents`/
`pnl_unrealized_cents` fields are computed by summing `markets()` at publish time
instead (dashboard-poll-rate, not T2-rate, so an O(64) scan there is free) — the two
computations are independently verified against each other by
`tests/test_telemetry.cpp`, not assumed to agree.

## Markets / current market / order book

| Frontend field | JSON path | Type | Update source | Authoritative? |
|---|---|---|---|---|
| Market list | `markets[]` | array | One entry per market Brain has touched | Yes |
| YES bid/ask | `markets[].yes_bid_cents`/`yes_ask_cents` | cents | `ShadowBook::best_bid`/`best_ask` | Yes |
| NO bid/ask | *(derived)* | — | `100 - yes_ask_cents` / `100 - yes_bid_cents` | **Derived, not fabricated** — this project's own documented normalization identity (`include/px/normalize.hpp`: `BUY NO @ p ≡ SELL YES @ (100-p)`), applied client-side, not a separate data source |
| Order book depth | `markets[].bid_depth[]` / `ask_depth[]` | `{price, qty}[]` | **New this pass.** Top `telemetry::kDepthLevels` (8) non-empty levels each side, scanned from `ShadowBook::level_qty` | Yes — real aggregate resting quantity per level, same figures `ShadowBook` itself holds |
| Market status | `markets[].status` | 0-3 | `Brain::market_status_`, republished every book event | Yes |
| Position | `markets[].position` | contracts | `PositionAccount::position()` | Yes |
| Avg entry | `markets[].avg_entry_cents` | cents | `PositionAccount::avg_entry_cents()` | Yes |
| Exposure | `markets[].exposure_cents` | cents | `\|position\| * 100` | Yes |

## Recent trades

| Frontend field | JSON path | Type | Update source | Authoritative? |
|---|---|---|---|---|
| Trade tape | `recent_trades[]` | `{ts_ms, market, side, price, qty}[]` | **New this pass.** `pipeline::BookEventKind::Trade`, Kalshi's own public trade feed | Yes — real market trades, NOT this account's own fills (those are `recent_orders`) |

## Positions / orders / execution

| Frontend field | JSON path | Type | Update source | Authoritative? |
|---|---|---|---|---|
| Positions table | `markets[]` (position/avg_entry/realized/unrealized/exposure columns) | — | Same as above | Yes |
| Orders table | `recent_orders[]` | `{ts_ms, market, id, kind, side, price, qty}[]` | `OrderTracker`/`ExecReport` lifecycle events | Yes |
| Order id | `recent_orders[].id` | number | **New this pass.** Brain's own `OrderId`, threaded into every order event | Yes — lets the dashboard group one order's own submit→ack→fill/cancel/reject events client-side, which the flat, previously-unlinked ring couldn't support |
| Order status | `recent_orders[].kind` | string | `submit`/`ack`/`fill`/`cancel`/`reject`/`risk_blocked` | Yes |

## Strategy activity

| Frontend field | JSON path | Type | Update source | Authoritative? |
|---|---|---|---|---|
| Decision feed | `recent_decisions[]` | `{ts_ms, market, strategy, price, qty, reason}[]` | `Strategy::on_event`'s returned `ExecutionDecision`, via `Brain::handle_decision` | Yes |
| Decision reason | `recent_decisions[].reason` | string | Free-form, e.g. `"imbalance=+0.71"` | Yes, but **not currently populated for any of the three live strategies** — see "Known gap" below |

## Risk

| Frontend field | JSON path | Type | Update source | Authoritative? |
|---|---|---|---|---|
| Max exposure (per-market cap) | `risk.max_exposure_cents` | cents | **New this pass.** `RiskConfig::max_exposure_cents`, published once at startup | Yes — but see "per-market, not global" note below |
| Fat-finger bounds | `risk.fat_finger_min`/`fat_finger_max` | cents | **New this pass.** `RiskConfig`, published once | Yes |
| Rate-limit headroom | `risk.rate_limit_tokens`/`rate_limit_capacity` | tokens | **New this pass.** Sampled from `RiskEngine::tokens_available()` on T2, alongside every risk check `Brain::handle_decision` already runs | Yes — a live figure, not a config value |
| Orders rejected (post-send) | `orders_rejected` | number | Kalshi rejecting an order Brain already sent | Yes |
| Orders blocked (pre-send) | `risk_rejections` | number | **New this pass.** Brain's own lifecycle gate or `RiskEngine::check`/`check_cancel` verdict, BEFORE anything was ever sent — see `telemetry::OrderEventKind::RiskBlocked`'s own comment for why this is kept distinct from `orders_rejected` rather than merged into one counter | Yes |

**"Per-market, not global" note**: `RiskConfig::max_exposure_cents` caps exposure
*per market* (`risk/risk_engine.hpp`'s `check()` compares one market's own projected
notional against it), not a portfolio-wide total. The dashboard's exposure gauge
therefore shows `max(each active market's own exposure_cents) / max_exposure_cents`
— the single most-at-risk market's utilization — not
`sum(all markets' exposure) / max_exposure_cents`, which would silently compare an
aggregate to a per-market limit and produce a number with no correct interpretation.

## What was deliberately NOT added this pass

Per the same Part 29 instruction ("do not fabricate it") — these would need either
inventing data with no real backing, or meaningfully more backend work than this
pass's scope, and are named explicitly rather than silently dropped:

- **Structured decision-feature breakdown** (e.g. separate `imbalance`/`spread`/
  `position` fields the redesign plan's own Strategy Activity example shows). The
  three live strategies (`strategy/passive_market_maker.hpp`,
  `strategy/order_book_imbalance.hpp`, `strategy/trade_flow.hpp`) never populate
  `ExecutionDecision`'s free-form `reason` string at all today (only
  `tests/test_telemetry.cpp`'s own hand-written test calls pass a non-null one, to
  exercise the plumbing) — the field exists and is wired end-to-end, but showing
  real per-decision reasoning needs each strategy's own `on_event` to start
  populating it, a Stage 3 code change out of this redesign pass's scope. The
  dashboard renders whatever the field actually contains (currently empty for a real
  run) rather than fabricating an explanation the backend never provided.
- **Full order-lifecycle stepper** (Intent → Gateway → Submitted → ACK → Partial
  Fill → Fill, each a distinct visualized stage). What's real: discrete lifecycle
  EVENTS (`recent_orders`, now linkable by `id` — see above). What's NOT real: a
  single object representing one order's current stage, or Gateway-internal
  sub-states (signing in flight, REST round-trip pending) — none of that crosses
  into telemetry, and fabricating intermediate stages between two real events would
  be exactly the kind of invented data this contract exists to rule out. The
  dashboard instead lists an order's own real events chronologically when selected —
  an honest, weaker version of the stepper the plan sketches.
- **Time-range selector beyond "Session"** (1H/6H/24H). `pnl_history` is a
  fixed-capacity in-memory ring (512 samples); there is no on-disk time-series store
  behind it. Retention is whatever 512 samples at the current sample rate covers
  (at least ~17 minutes at the throttled 1/2s book-update rate, unbounded longer if
  fills are sparse), not a guaranteed multi-hour window. Presented as "Session" only.
- **Live "rate limit OK" / "fat-finger OK" boolean status pills.** What's real:
  the actual token count (`risk.rate_limit_tokens`) and the actual configured bounds
  (`risk.fat_finger_min`/`max`) — both published. What's NOT added: a synthesized
  boolean "is everything fine right now" flag, since "fine" has no single
  well-defined threshold the backend itself asserts (rejections already show up
  individually via `risk_rejections`/`orders_rejected` and the order tape's
  `risk_blocked`/`reject` entries) — showing the real numbers and the real rejection
  events lets a viewer judge this themselves rather than trust an invented summary.

## Known gap: live fee tracking

`pipeline::ExecReport` (the live pipeline's own wire format, `pipeline/events.hpp`)
carries no fee field — `Brain::apply_exec_report` always calls
`telemetry_->on_order_filled(..., /*fee_cents=*/0, ...)` for a real fill. `fees_cents`
in the JSON is therefore always `0` for a live run, even though Kalshi genuinely
charges a fee on every fill (see `eval/fees.hpp`'s real fee formula, used only in the
backtest path). Already flagged as a concrete pre-real-capital blocker in
`docs/DECISION_GATE.md` — not fixed here; this contract just makes the same gap
visible from the dashboard-data side too, since it means `Net P&L`'s "after fees"
label is honest about the formula but the fee term inside it is currently always
zero for a real run.
