# Glossary

These are the terms a reader meets in exchange-lab, defined the way this project uses them. The glossary is for students and researchers working on market microstructure, trading strategies and controlled experiments. Where the project's meaning is narrower than the textbook one, the entry says so.

Two things to know before any definition:

- **Everything in `arena/` is simulated.** A session makes up its own nanoseconds. Its market is invented, not fitted to real data, so its numbers describe this instrument and not trading.
- **The main result is negative.** 10 µs of reaction delay is worth less than 24 seeds can resolve. 1 ms costs real money, but a null run with nobody informed keeps 96% of that effect, so the cost is competition for uninformed flow, not adverse selection. A positive control meant to calibrate the post-fill column then failed outright. Several entries below (*adverse markout*, *spread earned*, *positive control*) reflect that history. The README's [The first result is a negative one](README.md#the-first-result-is-a-negative-one) has the numbers.

How to read this file:

- Terms are grouped, and sorted alphabetically within each group.
- Code identifiers are in `code`.
- *Where* points at the file or doc that defines the term.

1. [Orders and the order book](#orders-and-the-order-book)
2. [Strategies and the reference market](#strategies-and-the-reference-market)
3. [Inside a session](#inside-a-session)
4. [Accounting](#accounting)
5. [Experiments and evaluation](#experiments-and-evaluation)
6. [The engine and its measurements](#the-engine-and-its-measurements)
7. [Words this project avoids](#words-this-project-avoids)

## Orders and the order book

**Ack** — The engine's acknowledgement that a new order passed admission. It comes before any trade the order causes. *Where:* `include/px/messages.hpp`.

**Admission before matching** — Every rejection is decided before any state changes: bad instrument, quantity, price or tick, duplicate id, or a full book. So an order is never partly executed and then rejected. *Where:* `README.md`; `CLAUDE.md`.

**Aggressor and resting side** — In a trade, the aggressor (taker) is the incoming order that crossed the book. The resting side (maker) is the order already there, and the fill executes at its price. Accounts count `maker_fills` and `taker_fills` on this basis, which has nothing to do with the strategy named `maker`. *Where:* `README.md`; `arena/accounting.hpp`.

**BookFull** — The rejection a GTC order gets when the engine cannot guarantee it a resting slot, for example a new price level on a side that is already full. A side holds 2,048 levels by default and 512 in the arena's engine. Any BookFull in a suite session fails the health gate. *Where:* `README.md`; `include/px/matching_engine.hpp`; `include/px/messages.hpp`; `arena/session.hpp`; `docs/RUNNER.md`.

**Cancel** — A request to remove a resting order by its id; only the owner may cancel it. The engine answers `Cancelled(User)`, or `Rejected(UnknownOrder)` if the id is not live or not yours. *Where:* `README.md`; `include/px/messages.hpp`.

**Event (`ExecEvent`)** — A 64-byte record of what the engine did: `Ack`, `Trade`, `Cancelled` (user, IOC remainder or self-trade) or `Rejected` (with a reason). Events are numbered by one engine-wide sequence and carry no timestamp. The event stream is the only thing a participant learns the market from. *Where:* `README.md`; `include/px/messages.hpp`; `arena/book_mirror.hpp`.

**GTC (good till cancelled)** — A limit order whose unfilled remainder rests in the book until it fills or is cancelled. It is the default for a strategy's `Action::limit`. *Where:* `include/px/messages.hpp`; `arena/strategy.hpp`.

**IOC (immediate or cancel)** — A limit order that trades what it can at once. Its remainder is cancelled (`Cancelled(IocRemainder)`) instead of resting. The `informed` strategy takes liquidity with IOC limits, so it never sweeps past the level it meant to hit. *Where:* `include/px/messages.hpp`; `arena/strategies/informed.hpp`.

**Limit order** — An order with a price limit: a buy trades against resting sells at or below it, and a sell against resting buys at or above it, best price first. Its price must be positive and on the tick grid, or it is rejected `InvalidPrice`. *Where:* `README.md`; `src/matching_engine.cpp`; `include/px/messages.hpp`.

**Market order** — An IOC sweep with no price limit. It trades against whatever is resting, and any remainder is cancelled. *Where:* `README.md`; `include/px/messages.hpp`.

**Mid** — The midpoint of the best bid and best ask, `(bid + ask) / 2` in integers. When either side is empty there is no mid, and such samples are counted as one-sided. *Where:* `arena/session.hpp`.

**Owner (`OwnerId`)** — The engine's identity for whoever submitted an order, used for cancels and self-trade prevention. In a session every participant is a distinct owner, numbered roster index + 1. *Where:* `include/px/types.hpp`; `arena/session.hpp`; `arena/strategies/registry.hpp`.

**Price and price units** — A price is a signed 64-bit integer that counts 1/10,000ths of a currency unit (`kPriceScale` = 10,000, so 1.0000 = 10,000). There is no floating point anywhere on the matching path. *Where:* `README.md`; `include/px/types.hpp`.

**Price level** — All resting orders at one price on one side. They form a FIFO queue, so a cancel is an O(1) unlink. Price, side and instrument are stored once on the level rather than on every order. *Where:* `README.md`; `include/px/sorted_levels.hpp`.

**Price-time priority** — The matching rule: the best price trades first, and within a level the oldest order fills first. A partial fill keeps the order's place, and fills execute at the resting order's price. *Where:* `README.md`.

**Queue position (`qty_ahead`)** — The quantity resting ahead of one of your orders at its own price. The engine never reports it; each participant's book mirror rebuilds it from the event stream. In this market the book is thin enough that time priority rarely binds. *Where:* `arena/book_mirror.hpp`; `README.md`.

**Self-trade prevention** — When an incoming order would trade against a resting order from the same owner, the engine cancels the resting order (`Cancelled(SelfTrade)`) and the sweep continues. This is why every participant must be a distinct owner: a shared owner would silently delete the maker's quotes instead of filling them. *Where:* `README.md`; `src/matching_engine.cpp`; `docs/ARENA.md`.

**Spread (quoted)** — The distance between the best ask and the best bid. The reference makers quote `half_spread_ticks` = 2 either side of their estimate, which gives the default market its 4-tick spread. This is a different thing from *spread earned* (see Accounting). *Where:* `arena/strategies/registry.hpp`; `arena/strategies/market_maker.hpp`; `README.md`.

**Tick (tick size)** — The smallest price step of an instrument, in price units; an off-tick price is rejected `InvalidPrice`. The arena's one instrument has a tick of 100 price units (0.01 of a currency unit). Strategies quote through `snap_to_tick`. *Where:* `README.md`; `arena/session.hpp`; `arena/strategy.hpp`.

**Touch** — The best bid and best ask together. When either changes, every strategy that has no event wake pending gets a `Touch` wake. *Where:* `arena/strategy.hpp`; `arena/session.hpp`.

## Strategies and the reference market

**Built-in and installed strategies** — A built-in strategy is compiled into every binary and listed in `kStrategyNames`: maker, rival, informed, noise, momentum, fader, placebo and roundtrip. An installed strategy is a header added with `make strategy`. It is compiled only into the lab binaries, so it cannot reach the binaries that produce published evidence. *Where:* `arena/strategies/registry.hpp`; `CLAUDE.md`.

**Candidate** — A strategy being scored by `make compare`. It sits in the seat, on every seed of a block, beside the placebo arm. One run takes 1 to 8 candidates, and `informed` and `placebo` are refused. *Where:* `docs/RUNNER.md`; `arena/suite.hpp`.

**Fader** — The worked example of a strategy written against `arena/strategy.hpp` alone. It keeps a slow average of traded prices and takes the other side at market when the last print is more than 6 ticks away from that average. It is uninformed and off by default, and in the suite it is inert on all 64 standard seeds. *Where:* `arena/strategies/fader.hpp`; `docs/ARENA.md`; `docs/RUNNER.md`.

**Glosten–Milgrom setting** — The market model the arena borrows. A dealer earns the spread from uninformed flow, pays it back to an informed trader, and infers value from the direction of order flow. The `maker` learns this way, from which side was the aggressor in each trade across the whole tape. *Where:* `README.md`; `docs/ARENA.md`; `arena/strategies/market_maker.hpp`.

**Imbalance** — The example strategy installed from outside the tree. It buys at market when the size at the best bid is at least 3 times the size at the best ask, and sells in the mirror case. It is uninformed and "an example, not a recommendation". Its header states the naming convention every installed strategy follows. *Where:* `examples/strategies/imbalance.hpp`.

**Informed** — The one participant given the fundamental value (`signal`), delivered through its own delayed feed. It takes a resting quote with an IOC limit when that quote is more than `edge_ticks` (1) on the wrong side of V. It reacts to V's current value and does not forecast it. The roster decides who is informed, never the strategy. *Where:* `arena/strategies/informed.hpp`; `arena/strategies/registry.hpp`; `README.md`.

**Inventory limit (`max_position`)** — The position past which a strategy stops adding to that side. It is load-bearing:
- at 500, a maker withdrew one side for 42% of the session, so the makers use 2,000;
- an informed limit of 1,000 let the book drift 26 ticks from V, so the informed trader uses 5,000.

*Where:* `arena/strategies/registry.hpp`; `docs/ARENA.md`.

**Inventory skew (`skew_ticks_at_limit`)** — How far a maker leans its quotes against its inventory. It is stated in ticks at the inventory limit, so the two parameters do not multiply. It is half of price discovery: at 4 the mean |mid − V| was 11 ticks, and at the default of 40 it is 3. *Where:* `arena/strategies/registry.hpp`; `arena/strategies/market_maker.hpp`.

**Learning rate (`impact_centiticks`)** — How far one full quote's worth of aggressive flow moves a maker's price estimate, in hundredths of a tick; the default is 15. Too low and the estimate never finds the price; too high and the makers chase uninformed flow. *Where:* `arena/strategies/registry.hpp`; `arena/strategies/market_maker.hpp`.

**Line-up (roster)** — The ordered list of participants in a session. Every published number came from the default line-up, `maker,rival,informed,noise:2,momentum`. `--strategies=` composes another line-up by name, and `:count` repeats an entry. *Where:* `arena/strategies/registry.hpp`; `docs/ARENA.md`.

**Maker** — The market maker the headline sweep varies. It quotes both sides 2 ticks either side of its own estimate, learns from the direction of flow on the whole tape, leans against its inventory, and never sees the fundamental. Its default reaction delay is 10,000 ns. *Where:* `arena/strategies/market_maker.hpp`; `arena/strategies/registry.hpp`; `docs/ARENA.md`.

**Momentum** — Buys or sells at market after 3 consecutive prints that moved the same way. Its information is second-hand: the informed trader's footprint, already in the price. Its own header calls it texture and the first strategy to delete. *Where:* `arena/strategies/momentum.hpp`.

**Noise** — Uninformed flow that acts only on its own timer. Each time it either posts a limit near the market, takes at the touch, or cancels an order. On average it loses the spread, and that is what pays the makers. The default line-up has two noise traders, on staggered 10 µs timers. *Where:* `arena/strategies/noise.hpp`; `arena/strategies/registry.hpp`.

**Participant** — One strategy instance in a session, with its own owner id, account, delayed feed and wakes; "participant" and "strategy" name the same thing. A session refuses more than 64 participants. The ceiling is hard because widening it would change wake ordering and move every session hash. *Where:* `arena/session.hpp`; `docs/ARENA.md`.

**Placebo** — The empty seat: a strategy that never acts, so its value is exactly zero on every seed. Appended last, it leaves the session hash unchanged. Inserted anywhere earlier it changes the hash, because every later owner id and reset seed shifts. *Where:* `arena/strategies/placebo.hpp`.

**Reference market** — The built-in default line-up that every candidate is measured against. Its participants have role `reference` in a run's files. *Where:* `CLAUDE.md`; `docs/RUNNER.md`.

**Rival** — A second copy of the maker strategy with a fixed fast delay (1,000 ns) that is never swept, so the swept maker has a competitor. With only one maker, slowing it down measures how much the market depends on it, not what its delay is worth. `--set=rival.enabled=0` reproduces that degenerate market. *Where:* `arena/strategies/registry.hpp`; `docs/ARENA.md`.

**Roundtrip** — The evaluator's positive control. On a 50 µs timer it buys 10 at market, then sells 10 at market, forever. It must lose money, and the loss must show up in spread earned. It is not to be tuned, and the size of its loss is reported, not predicted. *Where:* `arena/strategies/roundtrip.hpp`; `docs/CONTROLS.md`; `CLAUDE.md`.

**Script (`you`)** — A participant that plays a file of orders stamped in simulated time (`arena --script=PATH`); it is shown as `you`. Its reaction delay applies like anyone else's, and each scripted time is honoured at its first wake at or after that time. It replaces live keyboard trading, which would break replay. In the viewer, `f` re-reads the file and restarts the session rather than injecting the change into it. *Where:* `arena/strategies/script.hpp`; `docs/ARENA.md`.

**Strategy** — A plain C++ struct with five members (`name`, `reaction_ns`, `timer_ns`, `reset(seed)`, `on_event(view)`). It receives a `MarketView` and returns an `ActionBatch`, and it cannot touch the engine or read a clock. Strategies are compiled in, never loaded at runtime, and everything that takes part in a session is one. *Where:* `arena/strategy.hpp`; `docs/ARENA.md`; `CLAUDE.md`.

**Uninformed flow** — Trading by participants that cannot see the fundamental, above all the noise traders. It is the makers' income. Slowing the maker down mostly hands its share of this flow to the rival. *Where:* `arena/strategies/noise.hpp`; `README.md`.

## Inside a session

**ActionBatch** — What a strategy returns from one wake: up to six limit, market or cancel actions, which is enough to cancel and replace quotes on both sides. A seventh action is dropped and counted in `action_drops`. *Where:* `arena/strategy.hpp`.

**Book mirror** — A participant's own copy of the book: every level and every queue, rebuilt from the event stream alone. It is advanced only up to `now − reaction_ns`. The engine has no per-order query, and `test_arena` checks the mirror against the engine's own book after every request. *Where:* `arena/book_mirror.hpp`; `docs/ARENA.md`.

**Fundamental (V)** — The hidden value of the instrument. Starting from 100.00, it moves one tick up or down with equal probability at integer intervals averaging 100 µs. It is a martingale, seeded from the session seed alone. It is the only information in a session that does not come out of the order book, and every account is marked to it. *Where:* `arena/fair_value.hpp`; `arena/session.hpp`.

**In flight** — Orders a strategy has sent but not yet seen acknowledged (`MarketView::in_flight`). A strategy that ignores this field will quote twice. *Where:* `arena/strategy.hpp`; `arena/session.hpp`.

**MarketView** — Everything a strategy knows at a wake: `now_ns`, its book mirror as of `as_of_ns`, the wake reason, its own live orders with queue position, orders in flight, position, cash, fills, rejects and `signal`. It never contains the clock or other participants' positions. It contains the fundamental only for the informed participant. *Where:* `arena/strategy.hpp`.

**Price discovery** — How the book comes to track a value nobody is told. The makers move their estimates toward the side the flow comes from, and the informed trader's one-sided flow pulls them toward V. Together the two makers are the price-discovery mechanism. That is why a stale maker cannot be inserted as a probe without breaking the market it is supposed to measure. *Where:* `arena/strategies/market_maker.hpp`; `docs/ARENA.md`.

**Reaction delay (`reaction_ns`)** — A strategy's round-trip delay, which is an input the experimenter chooses. A strategy with delay d, woken by something at time t, acts at t + d and sees the market as of t. One setting therefore makes its view stale and its order late. In the suite the seat always runs at 10,000 ns, whatever the strategy declares. *Where:* `arena/clock.hpp`; `docs/ARENA.md`; `CLAUDE.md`.

**Reflecting wall** — A bound `band_ticks` (2,000) ticks from the start, where the fundamental is pushed back instead of stepping at random. Sessions are not expected to reach it. Each hit is counted in `wall_hits`, because the walk stops being a martingale there, and any hit fails the health gate. *Where:* `arena/fair_value.hpp`; `docs/RUNNER.md`.

**Scheduler** — The one queue that orders everything in a session by (time, sequence number). That is a total order, so no tie is ever broken by luck, and only the scheduler advances the clock. *Where:* `arena/clock.hpp`.

**Seed** — The number that fixes a session. It derives the fundamental's path and each strategy's reset seed (`seed + 0x9E37·(index + 1)`), so the same seed and line-up reproduce a session bit for bit. 42 is the repository's base seed. *Where:* `arena/session.hpp`; `docs/RUNNER.md`.

**Session** — One seeded run of the market. It holds an engine, the simulated clock, the fundamental, the roster and the accounting, and does no rendering. By default it ends after 200,000 engine requests; suite sessions end after 25 ms of simulated time instead. *Where:* `arena/session.hpp`; `docs/RUNNER.md`.

**Signal** — The fundamental as delivered to the informed participant, after that participant's own delay. It is 0 for everyone else. *Where:* `arena/strategy.hpp`; `arena/session.hpp`.

**Simulated time (`SimTime`)** — Nanoseconds since the start of a session, made up by the session rather than measured. No wall clock is read inside a session. *Where:* `arena/clock.hpp`; `README.md`.

**Stall (`stall_limit_ns`)** — A session that receives no request for 100 ms of simulated time ends and sets `stats().stalled`, which the sweep reports as a warning. Suite sessions turn this off and end on time alone, because a stall could end two arms at different times. *Where:* `arena/session.hpp`; `docs/ARENA.md`; `docs/RUNNER.md`.

**Viewer** — The terminal renderer, `arena`. It only reads a session: keys change pace and focus, never what happens. So a session watched in the viewer hashes the same as the same seed run headless. `--still=NS` prints one frame at a chosen simulated instant, which keeps the README picture reproducible. *Where:* `docs/ARENA.md`; `README.md`.

**Wake** — A moment when the scheduler calls a strategy's `on_event`. There are five reasons:
- Own: an event about one of its own orders.
- Touch: the best bid or ask changed.
- Timer: its `timer_ns` elapsed.
- Signal: the informed value moved.
- Open: one opening look at the start of the session.

At most one event wake is pending per strategy, so a burst of events inside one reaction window collapses into a single decision. *Where:* `arena/strategy.hpp`; `arena/session.hpp`.

## Accounting

**Adverse markout (`adverse`)** — The part of a fill's markout caused by the fundamental moving afterwards: `(V(t+H) − V(t)) · q`, with q positive for a buy. V is a martingale and no participant predicts it, so this column reads near zero for everyone by construction, and its ability to detect a real effect is not established. It is narrower than textbook "adverse selection": losses on stale quotes land in *spread earned*. *Where:* `arena/accounting.hpp`; `docs/ARENA.md`; `CLAUDE.md`.

**Attribution** — For each reference participant, the paired difference of its value between a candidate's arm and the placebo arm. The market is closed and fees are zero, so these differences sum to exactly minus the seat's value on every seed. That lets the report say who paid for a result. *Where:* `docs/REPORT.md`; `docs/RUNNER.md`.

**Carry to close** — What a fill's inventory earned between the end of its markout horizon and the close: `(V(T) − V(t+H)) · q`. It is computed as value − spread earned − adverse markout. *Where:* `docs/REPORT.md`; `arena/suite_report.hpp`.

**Cash** — An account's money, in price units times quantity. It goes negative when the account buys. *Where:* `arena/accounting.hpp`.

**Closed identity** — Every fill moves cash one way and quantity the other between two accounts. So with zero fees, the sums of cash, of position and of marked value over all participants are each exactly zero, in integers, at every instant. `test_arena` asserts it, and a suite session that breaks it makes the run exit 3. *Where:* `arena/accounting.hpp`; `README.md`; `docs/RUNNER.md`.

**Currency unit** — 10,000 price units. Arena tables report value in currency units per session, and the conversion happens only in reporting code. *Where:* `arena/accounting.hpp`; `include/px/types.hpp`.

**Drawdown (max drawdown)** — The largest fall of the seat's marked value from its running peak, with the peak starting at 0. It is sampled from outside the session each time the fundamental moves. *Where:* `docs/REPORT.md`.

**Edge** — A fill's whole markout, `(V(t+H) − fill price) · q`, which equals spread earned plus adverse markout exactly. Reporting edge alone was the first version's mistake: it made a slower maker look better, because a staler quote captures more spread on the fills it does get. *Where:* `arena/accounting.hpp`; `docs/ARENA.md`.

**Fees** — A flat per-unit fee, `session.fee_per_qty`, which is zero by default and in every suite. The closed identity is exact only at zero fees. *Where:* `arena/session.hpp`; `arena/accounting.hpp`; `docs/ARENA.md`.

**Fills per order** — Fills divided by orders sent. It is a count per order, not a percentage, because one aggressive order can fill against several resting ones. *Where:* `docs/REPORT.md`.

**Inventory** — The report's word for the seat's position over a session. The report gives final inventory, mean |inventory| and max |inventory|, from a path sampled each time the fundamental moves. *Where:* `docs/REPORT.md`.

**Maker share** — The percentage of an account's fills on which it was the resting side. *Where:* `docs/REPORT.md`; `arena/suite_report.hpp`.

**Marked value (value)** — `cash + position · V − fees`, reported at the closing fundamental V(T). Marking to V rather than to the mid stops a participant improving its own value by moving the price. Realised P&L alone is not used, because a losing maker could hide its loss by not closing its position. *Where:* `arena/accounting.hpp`; `docs/RUNNER.md`.

**Markout and markout horizon (H)** — A markout values a fill against the fundamental a fixed time H later. H is `session.markout_horizon_ns`, 1 ms by default, honoured to within one scheduler step. Fills still inside their horizon when a session ends are settled against the final V and counted as `unresolved_fills`. The report prints that count; in the standard report it was 3.9% of the seat's fills. *Where:* `arena/accounting.hpp`; `arena/session.hpp`; `docs/REPORT.md`.

**Position** — Signed quantity held, positive when long. *Where:* `arena/accounting.hpp`.

**Spread earned (`spread`)** — The part of a fill's markout earned at the instant of the fill: `(V(t) − fill price) · q`. Despite the name, this is where a stale quote's loss lands: a fill "at a price the fundamental has already left behind". It carries the result in both the headline sweep and the standard report. *Where:* `arena/accounting.hpp`; `docs/ARENA.md`; `README.md`; `docs/REPORT.md`.

## Experiments and evaluation

**A/A control** — `maker` run twice under two labels must come out identical on every seed. It catches pairing that is misaligned. *Where:* `docs/CONTROLS.md`.

**Admissible** — An arm is admissible only if every one of its sessions on the block passes the health gate. A comparison is admissible only if both its arms and the reference market are. Failing seeds are never dropped: the verdict becomes `not_admissible` and the numbers are still written. *Where:* `docs/RUNNER.md`; `arena/suite_report.hpp`.

**Arm** — One occupant of the seat (the placebo, or one candidate) run on every seed of a block. *Where:* `docs/RUNNER.md`.

**Book-to-fundamental gap (market health)** — The mean |mid − V| in ticks, sampled at every fundamental jump, with one-sided samples counted separately. It is printed before every result, because a book that has come loose from the fundamental is not a market, and nothing read off it counts. `docs/ARENA.md` calls the sweep's version the "health line". Measured values:
- The suite's placebo arm: median 1.51 ticks, worst 2.03.
- The published 200,000-request headline runs: 8–12 ticks, against a 4-tick spread.
- 16 of the 24 headline sessions at the 10 µs point are more than 4 ticks loose, which would fail the suite's gate.

*Where:* `arena/session.hpp`; `README.md`; `docs/RUNNER.md`; `CLAUDE.md`.

**Common close** — Every arm on a seed closes at the same fundamental, after the same number of fundamental steps. Ending sessions on simulated time is what guarantees this, and it is what makes pairing by seed valid. *Where:* `docs/CONTROLS.md`; `docs/RUNNER.md`.

**Controls** — The evaluator's eight checks whose answers are known in advance: placebo is zero, placebo is inert, common close, replay, closed identities, A/A, noise floor, and the positive control. `make controls` runs them. Each is paired with a planted defect in `test_arena` that must make it fail. *Where:* `docs/CONTROLS.md`.

**Evidence binaries and lab binaries** — `sweep`, `arena`, `suite`, `controls` and every test are built without installed strategies; they produce the published evidence and the pinned hashes. `sweep_lab`, `arena_lab` and `suite_lab` can also see installed strategies. *Where:* `Makefile`; `CLAUDE.md`.

**Fingerprint** — A 64-bit FNV-1a hash over a run's condition lines in `suite.cfg` and every placebo session hash. It moves if anything a candidate faced moves, and two results are comparable only if their fingerprints are equal. The smoke-block fingerprints of all three suites are pinned in `test_arena` (`standard`'s is `d461c30ab2446acf`). Moving one requires a suite version bump. *Where:* `arena/suite.hpp`; `docs/RUNNER.md`; `tests/test_arena.cpp`; `CLAUDE.md`.

**Headline sweep** — The experiment behind the project's result: the maker's reaction delay at 1 µs, 10 µs, 100 µs and 1 ms, on 24 seeds of 200,000 requests each. Every point runs on the same seeds, and every session is replayed. *Where:* `bench/run_arena_evidence.sh`; `README.md`.

**Health gate** — The suite's per-session admissibility test, decided in integers. A session passes only if all of these hold:
- the mean |mid − V| is at most 4 ticks;
- the book is one-sided at most 5% of the time;
- there are no BookFull rejects;
- there are no wall hits;
- the session ended on time;
- its replay or no-seat check was identical.

The sweep prints market health but gates nothing. *Where:* `arena/suite.hpp`; `docs/RUNNER.md`; `arena/sweep.cpp`.

**Inert** — A seat that sent no orders on a seed. Its arm must then hash the same as the placebo arm, and the report calls it inert rather than reporting zero edge. *Where:* `docs/RUNNER.md`.

**Interval (95%)** — The paired mean ± t × s.e., with Student's t at n − 1 degrees of freedom. The t values come from a fixed table, so every platform prints the same number. *Where:* `arena/suite_report.hpp`; `docs/RUNNER.md`.

**Many comparisons** — The report counts its interval tests and prints how many a 5% test would separate by chance alone (1.4 of 28 in the standard report). It applies no correction silently. *Where:* `docs/REPORT.md`.

**No-seat check** — Beside every placebo session, the suite runs the line-up with no seat at all. The two hashes must match; if they do not, the reference market is not admissible and the run exits 3. *Where:* `docs/RUNNER.md`; `docs/ARENA.md`.

**Noise floor** — The `maker` seat at 9,999 ns against the same seat at 10,000 ns, which must not separate, and does not: −0.06 ± 0.99, interval −2.04 to +1.92. A difference between two strategies smaller than about two currency units a session on the standard block should not be read as skill. *Where:* `docs/CONTROLS.md`.

**Null run** — `sweep --null`: the headline sweep with the only informed participant switched off, so speed should be worth nothing. It kept 96% of the 1 ms effect. That is how the project learned the cost is competition for uninformed flow. There is no `null` scenario family, because its placebo failed the health gate on 56 of 64 seeds. *Where:* `arena/sweep.cpp`; `README.md`; `docs/REPORT.md`.

**Paired difference** — The difference between two arms or two sweep points, taken seed by seed and then summarised. The seed fixes the fundamental's path, so pairing cancels most of the noise from leftover inventory marked at a random-walk price, the noise that makes raw levels unreadable. *Where:* `README.md`; `arena/sweep.cpp`; `arena/suite_report.hpp`.

**Pairing (variance removed)** — `1 − var(d) / (var(a) + var(b))`. It is printed for every candidate-against-candidate comparison because it varies so much: 85% for a one-parameter change, 23% for maker against imbalance, and −31% for noise against imbalance. *Where:* `arena/suite_report.hpp`; `docs/RUNNER.md`.

**Placebo arm** — The arm whose seat holds `placebo`. An inert seat appended last is invisible, so the placebo arm is exactly the reference market, and every difference between a candidate's arm and it is caused by the candidate's orders. *Where:* `docs/RUNNER.md`; `docs/ARENA.md`.

**Planted defect** — A deliberate bug in `test_arena` that must make a named control fail; a control that cannot fail proves nothing. The defects include pairing off by one seed, a request bound, the placebo inserted early, `static` state, and a value off by one. *Where:* `docs/CONTROLS.md`.

**Positive control** — A run built so the measure must find an effect. There are two, with opposite outcomes:
- The arena's stale-maker control **failed**: all three designs broke the book's tracking of the fundamental, so the post-fill column was never calibrated.
- The evaluator's `roundtrip` control **holds**: −79.55 ± 0.17 a session, almost all of it in spread.

Nothing about the second redeems the first. *Where:* `docs/ARENA.md`; `docs/CONTROLS.md`; `CLAUDE.md`.

**Range straddles zero** — The sweep's own separation test: a paired difference counts only if its full min-to-max range over the seeds excludes zero. It is far stricter than an interval from the standard error. The 100 µs point fails it while sitting about six standard errors from zero, so both tests are reported. *Where:* `arena/sweep.cpp`; `docs/ARENA.md`.

**Replay check** — Running a session again from fresh instances and requiring the same session hash. `sweep --replay-check` does this for every sweep session, and the suite does it for every candidate session, exiting 1 on a difference. It also catches a strategy that carries `static` state from one session into the next. *Where:* `arena/sweep.cpp`; `docs/RUNNER.md`.

**Reproduce** — `make reproduce RUN=runs/x` re-runs a run from its own `suite.cfg`. It refuses before running if any condition line or an installed strategy's checksum differs. It then compares every file byte for byte and prints `REPRODUCED` or `NOT REPRODUCED`. The `run.*` and `build.*` lines are recorded but are not conditions. *Where:* `docs/REPRODUCE.md`.

**`results/` and `runs/`** — `results/` holds the committed, published evidence, regenerated by scripts and never edited by hand. `runs/` is git-ignored and holds suite run directories. Their sessions last 25 ms, which makes them a different quantity, so they never go in `results/`. *Where:* `CLAUDE.md`; `README.md`.

**Robustness sweeps** — Four sweeps of other parameters (informed edge, maker spread, noise arrival rate and learning rate), published whatever they show. A shape that exists at only one set of parameters is not a finding. *Where:* `README.md`; `bench/run_arena_evidence.sh`.

**Run directory** — What one suite run writes:
- `suite.cfg`: the conditions;
- `sessions.csv` and `participants.csv`: the raw results;
- `comparison.csv`, `evaluation.csv`, `attribution.csv` and `report.txt`: derived from the raw files;
- `outcome.cfg`: written last.

A directory without `status=complete` in `outcome.cfg` is incomplete. *Where:* `CLAUDE.md`; `docs/RUNNER.md`.

**Scenario family** — A suite of its own that changes one thing about the market and has its own fingerprint, placebo arm and health gate. Beside `standard` there are `wide` (`maker.half_spread_ticks=4`) and `busy` (`noise.timer_ns=5000`). Families are reported side by side and never pooled. *Where:* `arena/suite.hpp`; `docs/REPORT.md`.

**Seat** — The slot a candidate occupies: index 6, owner 7. It is appended after the line-up, so no reference participant's owner id or random stream changes. It is never informed, and the suite sets its reaction delay at 10,000 ns. A strategy that reads its own `reaction_ns` sees what it declared; its real delay is `now_ns − as_of_ns`. *Where:* `docs/RUNNER.md`; `arena/suite.hpp`; `CLAUDE.md`.

**Seed block** — A named set of seeds; there is no free seed list. `standard` is 42..105 (64 seeds). `smoke` is its prefix, 42..45: it shows that a strategy runs, and cannot separate anything. *Where:* `arena/suite.hpp`; `docs/RUNNER.md`.

**Smallest resolvable effect (`resolvable`)** — About 2.8 × the paired standard error: the smallest effect a given number of seeds could have resolved at roughly 95% confidence. It is what tells "underpowered" apart from "nothing there". *Where:* `bench/summarize_arena.py`; `docs/RUNNER.md`.

**Standard error (paired)** — The standard deviation of the per-seed differences, divided by √n. Every arena result is reported with it, negative ones especially. *Where:* `bench/summarize_arena.py`; `docs/ARENA.md`; `CLAUDE.md`.

**Step ceiling** — A bound of 2,000,000 scheduler steps on a suite session, as protection against a runaway strategy. A session that reaches it is marked `ended_by=ceiling` and fails the health gate. It is not a stopping rule. *Where:* `docs/RUNNER.md`; `arena/suite.hpp`.

**Suite** — A named, versioned set of conditions defined in code (`SuiteDef`): the line-up, the session length (25 ms), the seat delay, the step ceiling, the health gate and every reference parameter. Candidates are scored on `standard` v1. A different set of conditions is a new suite, never a command-line flag. *Where:* `arena/suite.hpp`; `docs/RUNNER.md`.

**Sweep** — `arena/sweep.cpp`: the same seeded market run at several values of one parameter, with several seeds per value. It writes CSV and reports paired differences against the first value. The headline, null and robustness runs are all sweeps. *Where:* `arena/sweep.cpp`; `bench/run_arena_evidence.sh`.

**Underpowered** — A difference smaller than the seeds could resolve. It is reported as "not established", which is a different claim from "nothing there". The 10 µs point is underpowered at 24 seeds. *Where:* `README.md`; `docs/ARENA.md`.

**Uptime cost and selection cost** — The two ways being slow can cost a maker. The uptime cost is failing to keep a quote standing (fewer fills, less spread earned). The selection cost is being picked off on the quotes it does have. In this market the uptime cost dominates by roughly three orders of magnitude, the reverse of what the experiment was designed to show. *Where:* `README.md`; `docs/ARENA.md`.

**Verdict** — The label on each paired comparison in `comparison.csv`:
- `not_admissible`;
- `identical`: every per-seed difference is exactly 0;
- `separated`: the 95% interval excludes 0;
- `not_separated`.

"Separated" in the suite means this interval test, not the sweep's range test. *Where:* `docs/RUNNER.md`; `arena/suite_report.hpp`.

## The engine and its measurements

**Determinism** — The contract that the same input gives a bit-identical event stream on every OS. For the engine it is held by pinned event hashes at seed 42: pipeline `63bb035a395bae79` (thin) and `75ec1ab46ce5260a` (deep), and `test_determinism` `77a560d8ffb4fa43`. The arena keeps it with integer-only sessions and the (time, sequence) order. Its committed evidence is identical on Windows and Linux to the last digit. *Where:* `CLAUDE.md`; `README.md`; `arena/clock.hpp`.

**Event hash** — `EventHasher` folds every event in field by field, applying FNV-1a to each field's value rather than to the raw bytes, so struct padding cannot leak in. It is how determinism is checked. *Where:* `include/px/messages.hpp`.

**Gate (the engine's)** — The fixed pass mark for a latency run:
- p99.9 under 2 µs;
- zero outbound-ring stalls;
- zero allocations in the recorded window.

The count of orders over 2 µs and the slowest order are always printed, and nothing is trimmed. Paced runs fail this gate on the development laptop, and the README says so. It is unrelated to the suite's health gate. *Where:* `README.md`; `CLAUDE.md`.

**Matching engine** — The single-threaded core (`MatchingEngine::process`) that validates, admits, matches and rests orders. It has no locks and makes no allocation after construction. The arena is one more caller of it and adds no engine API. *Where:* `README.md`; `CLAUDE.md`; `arena/session.hpp`.

**Matching latency** — The engine's measured time per order: from dequeuing it to writing its last resulting event to the outbound ring, timed on the matcher thread. It is the only thing in the project called latency. *Where:* `README.md`; `CLAUDE.md`.

**Replay (best of K)** — For the engine's benchmarks, the same stream replayed K times on fresh engines. Every event stream must be bit-identical. Each order's fastest time estimates its true engine cost, because timing noise only ever adds time. *Where:* `README.md`.

**Service time** — What matching latency measures: the engine's own work on an order. It excludes time spent waiting in the inbound ring, and there is no network, parsing or NIC in it. *Where:* `README.md`; `CLAUDE.md`.

**Session hash** — One number per arena session, mixing the engine's event hash with the arrival time of every event. The same seed and line-up give the same hash. `sweep` prints it and `test_arena` asserts it, but it is deliberately not pinned, because any change to a strategy or a default moves it. *Where:* `arena/session.hpp`; `CLAUDE.md`.

**SPSC ring** — The lock-free single-producer/single-consumer ring of 64-byte slots that carries requests into the matcher thread and events out of it. *Where:* `include/px/spsc_ring.hpp`; `README.md`.

**Timer floor** — The cost of an empty timestamp bracket, about 32 ns. It is printed with every run and never subtracted. *Where:* `README.md`.

**Zero allocation** — The engine's guarantee that `process()` makes no heap allocation after construction, held at 0 by `test_alloc_audit` across 1.1M calls. It deliberately stops at `arena/`, which allocates freely and is never timed. *Where:* `README.md`; `CLAUDE.md`; `docs/ARENA.md`.

## Words this project avoids

**"adverse selection" for the `adverse` column** — Say *adverse markout* or *post-fill drift*. The column measures only the fundamental's move after a fill, and it reads near zero by construction. The textbook effect, being filled on a stale quote, lands in `spread`. So reading zero there as "nothing adverse happened" is wrong. *Why:* `CLAUDE.md`; `docs/ARENA.md`.

**"backtester"** — Say *simulated market* or *research platform*. A backtester replays historical market data, and nothing here does:
- the only outside input is a seeded random walk (`arena/fair_value.hpp`);
- live market data is out of scope (`docs/ARENA.md`);
- the other participants react to your orders, so there is no fixed tape to replay (`docs/RUNNER.md`).

The market is invented, not fitted to data. Its results are "a statement about this instrument at this seed count, not about trading" (`README.md`), and they do not predict real P&L.

**"bot"** — Say *strategy*. Everything in a session, built-in or installed, is a strategy, and the built-in ones are the reference market (`CLAUDE.md`; `docs/RUNNER.md`). A strategy is a plain struct compiled into the binary, and it trades only inside a simulated session (`arena/strategy.hpp`; `docs/ARENA.md`). Nothing in the tree connects to an exchange (`CLAUDE.md`).

**"fill rate"** — Say *fills per order*. One aggressive order can fill against several resting ones, so the ratio is not a share of orders filled; the first run printed 115.58%. *Why:* `docs/REPORT.md`.

**"latency" for anything in `arena/`** — Say *reaction delay* (`reaction_ns`). The engine's latency is measured on hardware; a reaction delay is a made-up input, and the two kinds of number must never be read together. The same rule is why the repository's name avoids the word. *Why:* `CLAUDE.md`; `docs/ARENA.md`; `arena/clock.hpp`.

**"tick-to-trade"** — Say *service time*. The engine's timing starts after an order is dequeued and leaves out queueing, network, parsing and the NIC. Calling it tick-to-trade or wire-to-wire would be a false claim. *Why:* `CLAUDE.md`; `README.md`.