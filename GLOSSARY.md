# Glossary

The terms you meet in exchange-lab, defined the way this project uses them. Where the project's meaning is narrower than the textbook one, the entry says so. Two things to keep in mind throughout:

- **Everything in `arena/` is simulated.** A session makes up its own nanoseconds, and its market is invented. Calibrated to a data file, it matches three of that file's statistics and nothing else, so its numbers describe this instrument and not trading.
- **The main result is negative.** 10 µs of reaction delay is worth less than 24 seeds can resolve; 1 ms costs real money, but a null run with nobody informed keeps 96% of that effect, and a positive control meant to calibrate the post-fill column failed outright. The README's [The first result is a negative one](README.md#the-first-result-is-a-negative-one) has the numbers.

Terms are grouped and sorted alphabetically within each group.

1. [The app](#the-app)
2. [Orders and the order book](#orders-and-the-order-book)
3. [Strategies and the reference market](#strategies-and-the-reference-market)
4. [Inside a session](#inside-a-session)
5. [Accounting](#accounting)
6. [Experiments and evaluation](#experiments-and-evaluation)
7. [Market data](#market-data)
8. [The engine and its measurements](#the-engine-and-its-measurements)
9. [Words this project avoids](#words-this-project-avoids)

## The app

**exchange-lab** — The project's terminal app: one prompt, driven by slash commands such as `/compare` and `/watch`, which runs the project's existing programs underneath. *Where:* `docs/APP.md`.

**Lab programs** — `sweep_lab`, `suite_lab`, `arena_lab` and `replay`, the only programs that can see an installed strategy. The evidence programs (`sweep`, `arena`, `suite`, `controls`, the tests) cannot, so a stranger's header never reaches published evidence.

**Slash command** — A line typed at the app's prompt, starting with `/`, that does one fixed thing: `/compare` scores strategies, `/watch` opens the viewer.

## Orders and the order book

**Ack** — The engine's acknowledgement that a new order passed admission, sent before any trade it causes.

**Admission before matching** — Every rejection is decided before any state changes, so an order is never partly executed and then rejected.

**Aggressor and resting side** — The aggressor (taker) crossed the book; the resting side (maker) was already there and sets the fill price. This has nothing to do with the strategy named `maker`.

**BookFull** — The rejection a GTC order gets when the engine cannot guarantee it a resting slot.

**Cancel** — A request to remove a resting order by its id; only its owner may make it.

**Event (`ExecEvent`)** — A 64-byte record of what the engine did (`Ack`, `Trade`, `Cancelled`, `Rejected`), sequence-numbered and without a timestamp. It is the only thing a participant learns the market from.

**Limit order** — An order with a price limit, trading best price first. A GTC one rests its remainder; an IOC one cancels it.

**Market order** — An IOC sweep with no price limit.

**Mid** — The midpoint of the best bid and ask; with either side empty there is none, and the sample counts as one-sided.

**Owner (`OwnerId`)** — Whoever submitted an order, used for cancels and self-trade prevention. Every participant is a distinct owner.

**Price and price units** — A price is a 64-bit integer counting 1/10,000ths of a currency unit; there is no floating point on the matching path.

**Price level** — All resting orders at one price on one side, in a FIFO queue.

**Price-time priority** — The best price trades first, and within a level the oldest order.

**Queue position (`qty_ahead`)** — The quantity resting ahead of your order at its price, rebuilt by each participant's book mirror because the engine never reports it.

**Self-trade prevention** — An order that would trade against its own owner's resting order cancels that order instead, which is why every participant must be a distinct owner.

**Spread (quoted)** — Best ask minus best bid; the default market's is 4 ticks. Not the same as *spread earned*.

**Tick** — The smallest price step; an off-tick price is rejected.

**Touch** — The best bid and best ask together.

## Strategies and the reference market

**Built-in and installed strategies** — Built-in ones are compiled into every program and listed in `kStrategyNames`; installed ones are headers added with `/add` (`make strategy`), compiled only into the lab programs.

**Candidate** — A strategy being scored by `/compare`, in the seat on every seed of a block.

**Coding agent** — An AI tool that writes code and runs commands, used beside exchange-lab to write a strategy or read a report. It may write a strategy; no model may run inside a session. *Where:* `docs/CODING-AGENT.md`.

**Fader** — The worked example of a strategy: it fades prints that stray far from a slow average. *Where:* `arena/strategies/fader.hpp`.

**Frozen model** — A model trained outside the project whose weights are rounded to integers and written into a strategy header, so it runs as a pure integer function of the market view and replays like any strategy. *Where:* `examples/strategies/frozen.hpp`, `examples/frozen/README.md`.

**Glosten–Milgrom setting** — The model the arena borrows: a dealer earns the spread from uninformed flow, pays it to an informed trader, and infers value from order flow.

**Imbalance** — The example strategy installed from outside the tree; "an example, not a recommendation". *Where:* `examples/strategies/imbalance.hpp`.

**Informed** — The one participant shown the fundamental, as a delayed `signal`; it takes quotes on the wrong side of V without forecasting it.

**Inventory limit, inventory skew and learning rate** — The maker's load-bearing parameters: how much it may hold, how far it leans against inventory, and how far flow moves its estimate. Changing one and reading only P&L gives a confident wrong answer.

**Line-up (roster)** — The ordered participants of a session; every published number used the default, `maker,rival,informed,noise:2,momentum`.

**Maker** — The market maker the experiments vary: it quotes around its own estimate, learns from flow and never sees the fundamental.

**Momentum** — Trades after consecutive prints move the same way, following the informed trader's footprint.

**Noise** — Uninformed flow on its own timer. It loses the spread on average, which pays the makers.

**Participant** — One strategy instance in a session, with its own owner, account, feed and wakes; a session refuses more than 64.

**Placebo** — The empty seat: a strategy that never acts, so its value is exactly zero.

**Reference market** — The built-in default line-up every candidate is measured against.

**Rival** — A second, fast maker that is never swept, so the swept maker has a competitor.

**Roundtrip** — The positive control: it buys then sells at market, and must lose money in spread earned. It is not tuned.

**Script (`you`)** — A participant playing a file of orders stamped in simulated time, in place of keyboard trading, which would break replay.

**Strategy** — A plain C++ struct that receives a `MarketView` and returns an `ActionBatch`, compiled in and never loaded at runtime. *Where:* `arena/strategy.hpp`.

**Uninformed flow** — Trading by participants that cannot see the fundamental: the makers' income.

**Zero intelligence (`zi`)** — The floor a candidate has to clear, after Gode and Sunder: random limit orders drawn uniformly around the mid, one at a time, within a position budget, with no view at all. It loses about 92 a session on the standard block, mostly in spread. *Where:* `arena/strategies/zi.hpp`, `docs/RUNNER.md`.

## Inside a session

**ActionBatch** — Up to six actions a strategy returns from one wake.

**Book mirror** — A participant's copy of the book, rebuilt from events and advanced only to `now − reaction_ns`.

**Fundamental (V)** — The instrument's hidden value, a seeded martingale random walk; every account is marked to it.

**In flight** — Orders sent but not yet acknowledged; ignoring them means quoting twice.

**MarketView** — Everything a strategy knows at a wake: time, book mirror, own orders, position, cash, fills.

**Price discovery** — How the book tracks a value nobody is told: makers follow flow, and the informed trader's flow pulls them toward V. A stale maker therefore cannot be a probe without breaking the market.

**Reaction delay (`reaction_ns`)** — A chosen input: woken at t, a strategy acts at t + d on the market as of t. The suite's seat always runs at 10,000 ns.

**Seed** — The number that fixes a session; the same seed and line-up reproduce it bit for bit.

**Session** — One seeded run of the market. Default sessions end after 200,000 requests, suite sessions after 25 ms of simulated time.

**Simulated time** — Nanoseconds made up by the session; no wall clock is read inside one.

**Viewer** — The terminal renderer behind `/watch`, the `arena` program. It only reads a session, so watching never changes the hash.

**Wake** — A call to a strategy: for its own event, a touch change, its timer, a signal move or the open. Bursts within one reaction window collapse into one wake.

## Accounting

**Adverse markout (`adverse`)** — The part of a fill's markout from V moving afterwards. It reads near zero by construction; stale-quote losses land in *spread earned*.

**Attribution** — Each reference participant's paired change in value between a candidate's arm and the placebo arm, which together sum to minus the seat's value.

**Carry to close** — What a fill's inventory earned after its markout horizon, up to the close.

**Cash and position** — An account's money and its signed quantity held.

**Closed identity** — With zero fees, cash, position and value each sum to exactly zero over all participants.

**Drawdown** — The seat's largest fall in marked value from its running peak.

**Edge** — A fill's whole markout, spread earned plus adverse markout. Reported alone, it made a slower maker look better.

**Fills per order** — Fills divided by orders sent; not a percentage.

**Inventory** — The seat's position over a session: final, mean and max size.

**Maker share** — The percentage of fills on which an account was the resting side.

**Marked value (value)** — Cash plus position at the closing fundamental, minus fees, so moving the price cannot improve it.

**Markout and horizon (H)** — A fill valued against V a fixed time H later, 1 ms by default; fills unresolved at the close are counted.

**Spread earned (`spread`)** — The part of a fill's markout earned at the instant of the fill. Despite the name, a stale quote's loss lands here.

## Experiments and evaluation

**A/A control** — `maker` run twice under two labels must come out identical.

**Admissible** — An arm whose sessions all pass the health gate; failing seeds are never dropped.

**Arm** — One occupant of the seat run on every seed of a block.

**Book-to-fundamental gap (market health)** — The mean |mid − V| in ticks, printed first: a book loose from the fundamental is not a market.

**Common close** — Every arm on a seed closes at the same fundamental, which makes pairing valid.

**Controls** — The evaluator's checks with known answers, each paired with a planted defect that must make it fail. *Where:* `docs/CONTROLS.md`.

**Fingerprint** — A hash of a run's conditions; results are comparable only if fingerprints match, and moving a pinned one needs a suite version bump.

**Health gate** — The suite's per-session test: book close to V and two-sided, no BookFull, ended on time, replay matched.

**Holdout block** — `holdout`, seeds 1,000,000..1,000,063: a confirmatory block shared with no other and used by no example. Tune on `standard`, then report once on `holdout`, beside the standard run and never pooled with it. *Where:* `docs/RUNNER.md`.

**Holm correction** — A step-down adjustment of p-values that keeps the chance of any false separation among a run's value comparisons at 5%. Printed beside the raw intervals when a run has more than one value comparison, and across every cell of `families.txt`; the metric rows are not corrected. *Where:* `docs/REPORT.md`.

**Inert** — A seat that sent no orders; its arm must equal the placebo arm.

**Interval (95%)** — The paired mean ± t × standard error.

**Many comparisons** — The report prints how many separations chance alone would produce, and a Holm correction across the value comparisons.

**No-seat check** — Each placebo session must hash the same as the line-up without a seat.

**Noise floor** — The seat at 9,999 ns against 10,000 ns must not separate; differences under about two currency units a session are not skill.

**Null run** — `/sweep --null`, with nobody informed. It kept 96% of the 1 ms effect, so the cost is competition for uninformed flow.

**Paired difference** — A difference taken seed by seed, which cancels most of the noise.

**Pairing (variance removed)** — How much noise pairing cancelled; it varies widely and can be negative.

**Placebo arm** — The arm with `placebo` in the seat: exactly the reference market.

**Planted defect** — A deliberate bug that must make a named control fail; a control that cannot fail proves nothing.

**Positive control** — A run built so the measure must find an effect. The arena's stale-maker control failed; `roundtrip` holds and does not redeem it.

**Range straddles zero** — The sweep's test: a difference counts only if its min-to-max range over seeds excludes zero. It is far stricter than an interval.

**Replay check** — Re-running a session from fresh instances and requiring the same hash.

**Reproduce** — `/reproduce` (`make reproduce RUN=runs/x`) re-runs a run from its `suite.cfg` and compares every file. *Where:* `docs/REPRODUCE.md`.

**`results/` and `runs/`** — `results/` is committed, script-generated evidence; `runs/` is git-ignored suite output, never moved into `results/`.

**Run directory** — One suite run's files, ending with `outcome.cfg`; without `status=complete` it is incomplete. *Where:* `docs/RUNNER.md`.

**Scenario family** — A suite changing one thing about the market (`wide`, `busy`), reported beside the others, never pooled.

**Seat** — The slot a candidate occupies, appended after the line-up and never informed. Its real delay is `now_ns − as_of_ns`, whatever the strategy declares.

**Seed block** — A named set of seeds: `standard` is 42..105; `smoke`, 42..45, shows a strategy runs; `holdout` is the confirmatory block.

**Sensitivity protocol** — `/compare NAME sensitivity`: the run repeated under a fixed grid of conditions (the seat's delay, the amount of noise flow, the line-up), each a suite of its own, never pooled. A result holds if it separates with standard's sign in every admissible condition. *Where:* `docs/RUNNER.md`.

**Smallest resolvable effect (`resolvable`)** — About 2.8 × the paired standard error.

**Standard error (paired)** — The standard deviation of per-seed differences over √n; every arena result carries it.

**Suite** — A named, versioned set of conditions defined in code; a different set is a new suite, never a flag.

**Sweep** — One parameter varied over several values on shared seeds.

**Underpowered** — Smaller than the seeds could resolve: "not established", not "nothing there".

**Verdict** — The label on each comparison: `not_admissible`, `identical`, `separated` (the 95% interval excludes 0) or `not_separated`.

## Market data

**Calibrated market** — The standard market with its makers' half spread, `noise.qty` and `noise.timer_ns` set by `/calibrate`, written to `markets/NAME.cfg` and used with `market=NAME`. A scenario family of its own with its own fingerprint, run only by the lab programs, and reproducible from a run's own record without the data. *Where:* `docs/DATA.md`.

**Calibration** — Setting the simulated market so three statistics of its sessions match a tape's: mean spread, mean print against mean touch depth, and prints per move. Ratios, not the clock: real seconds are never matched. Everything else, the informed trader above all, keeps the standard market's value. *Where:* `docs/DATA.md`.

**Depth (of a tape)** — How much a data file holds: order by order (L3, every order), price levels (L2, the size at each price) or trades only. Calibration needs L2 or L3; replay needs L3.

**Held-back part (`holdback=`)** — The last part of a tape, by events, that `/calibrate NAME holdback=PCT` keeps from the search and then measures, beside how much the tape's own two parts differ. *Where:* `docs/DATA.md`.

**History cannot react** — The label replay leads with: a recorded order arrives when it did, whatever the strategy does, so what a strategy earns in a replay is the most the recorded flow could have given it, not what would have happened.

**Identification** — Whether one parameter set alone fits a tape. `/calibrate` lists every other set the search ran whose matched statistics are within two standard errors of the chosen one's; any listed means the tape cannot tell them apart.

**Importer** — The code that reads one outside format (LOBSTER, Databento, Tardis, Binance, Coinbase, Kraken, ITCH, any CSV) into a tape. Written from each format's published documentation and tested on hand-made samples in `examples/data/`.

**Known-answer control (calibration's)** — Sessions run with known parameters are written as a tape, and calibrating from it must give each parameter back within one step of its grid; a reader that drops every cancel, or misreads the price scale, must make it fail. `/calibrate` runs it first and refuses if it does not hold. From trades alone it does not hold, so a trades-only tape is refused.

**Print** — The trades one aggressor made at one moment, summed; the unit calibration counts.

**Prints per move** — How many prints it takes the mid to move one tick: 20 over the mean squared change of the mid, in ticks, across every run of 20 prints. Calibration's measure of pace.

**Replay (of a tape)** — `/replay NAME`: an order-by-order tape played back through the matching engine, with one strategy trading in it. With no strategy it is a control: the replayed book must equal the tape's own after every event. *Where:* `docs/DATA.md`, "Replay".

**Stylised facts** — The statistics a market model is usually checked against (return and |return| autocorrelation, fat tails, volume against volatility, spread and depth distributions, order flow), printed by `/facts` and `/calibrate` beside the simulated market's, read in intervals of 10 of each tape's own mean times between prints. A diagnostic: never fitted, never a pass or a fail. *Where:* `docs/DATA.md`.

**Tape** — The one internal format every data file is converted to, in `data/NAME.tape`: integer columns of time, event (add, cancel, trade, level, reset), order id, side, price in ticks and quantity, behind a header with the source's `cksum`. Never committed. *Where:* `docs/DATA.md`.

## The engine and its measurements

**Canonical record** — `make canonical`: one line for every output that must be identical on every platform (engine and session hashes, suite fingerprints, tapes, a smoke run's files), diffed with `tests/canonical.txt`. CI runs it on five targets. Lines labelled `derived.*` hold `double` statistics, so arithmetic is told apart from market state. *Where:* `docs/PLATFORMS.md`.

**Determinism** — The same input gives a bit-identical event stream on every OS, checked by pinned event hashes.

**Floating-point contraction** — A compiler fusing `a*b+c` into one multiply-add that rounds once instead of twice. It changes the last digit of the report's statistics, so it is turned off for every compiler. *Where:* `docs/PLATFORMS.md`.

**Gate (the engine's)** — p99.9 under 2 µs, zero ring stalls, zero allocations, nothing trimmed.

**Matching engine** — The single-threaded core that validates, admits, matches and rests orders; the arena is one more caller.

**Matching latency (service time)** — The engine's measured time per order, from dequeue to its last event written, excluding queueing. The only thing called latency here.

**Replay (best of K)** — The same benchmark stream replayed on fresh engines; each order's fastest time estimates its cost.

**Session hash** — A hash of an arena session's events and their arrival times; not pinned.

**SPSC ring** — The lock-free single-producer/single-consumer ring carrying requests in and events out.

**Timer floor** — The cost of an empty timestamp bracket, about 32 ns, printed and never subtracted.

**Zero allocation** — `process()` allocates nothing after construction; `arena/` is exempt.

## Words this project avoids

**"adverse selection" for the `adverse` column** — Say *adverse markout*; the textbook effect lands in `spread`.

**"backtester"** — Say *simulated market*. The one place historical data is played back is `/replay`, which says first that history cannot react.

**"bot"** — Say *strategy*.

**"fill rate"** — Say *fills per order*.

**"latency" for anything in `arena/`** — Say *reaction delay*, a made-up input.

**"tick-to-trade"** — Say *service time*.
