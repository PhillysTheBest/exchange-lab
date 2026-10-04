# Arena — plan

A sandbox where bots trade against each other inside this matching engine, rendered
live in the terminal as a retro desktop. Built on the existing engine; nothing here
changes the hot path.

## Why

The engine is finished and proven, but a reader cannot *see* anything: the repo is
numbers and tests. The arena makes the exchange watchable and playable, and turns the
project from "a matching engine" into "an exchange and the lab where strategies and
speed compete". It also brings back the market-making work from the old project, which
is still in git history (`git show 07b3be0^:strategy/passive_market_maker.hpp`), onto a
venue where fills are exact instead of estimated — the flaw that stopped that work
before.

## The boundary: quant, not full stack

Everything compiles into one binary. Nothing talks over a network.

| In scope | Out of scope |
|---|---|
| Bots as C++ headers, registered at compile time | Web server, REST, JSON, sockets |
| Deterministic, seeded, replayable sessions | Database, accounts, auth |
| P&L, inventory, adverse selection, queue position | npm, bundlers, a browser |
| One dependency-free terminal renderer | Plugin DLLs, an embedded scripting language |
| Per-bot latency, scheduled in simulated time | Live market data, broker APIs |

"Adding your own bot" means: write a header in `arena/bots/`, add one line to the
registry, rebuild. That is how trading research platforms link strategies internally,
and it keeps the project a systems-and-strategy project rather than a web project.

## What it looks like

A purple grid desktop of chunky windows with magenta/teal/orange title bars, hard drop
shadows and block-character meters, drawn with 24-bit ANSI colour — the attached mock,
rendered in a terminal. No curses, no libraries: one `ui.hpp` that owns a grid of
coloured cells and writes each frame in a single call.

Windows on the desktop, each mapping to real engine state:

| Window | Shows |
|---|---|
| **ORDER BOOK** (teal) | The depth ladder: asks above, bids below, size as horizontal bars, best prices meeting in the middle |
| **TAPE** (magenta) | Trades as they print: price, size, aggressor side |
| **PARTICIPANTS** (violet) | One row per bot: position, realised P&L as a signed bar, fill count |
| **LATENCY** (orange) | Each bot's reaction time as one of the mock's sliders — this is the dial the experiment turns |
| **SESSION** (teal) | Seed, orders processed, a progress bar, speed control |
| **ALERT** (magenta dialog) | Pops up on notable events: a sweep that clears several levels, a bot hitting an inventory limit. Styled like the mock's WARNING box |

Keys: `space` pause, `+`/`-` speed, `1-9` focus a bot, `q` quit.

## Architecture

```
arena/ui.hpp         retro-desktop renderer: cells, windows, bars, sliders, dialogs.
                     Knows nothing about trading.
arena/arena.hpp      the session: owns the MatchingEngine, the clock, the scheduler,
                     the bots and the accounting. No rendering.
arena/bots/*.hpp     one header per bot.
arena/bots/registry.hpp   the single list of bots in play.
arena/arena.cpp      main(): wires session to renderer, handles keys.
```

The engine, `sim/` and the benchmarks are untouched. The arena is a new consumer of
the same `MatchingEngine`, so the latency numbers in the README stay valid and the
arena can never slow the measured path.

## The bot API

A bot sees a view of the market and returns actions. It never touches the engine.

```cpp
struct MarketView {
    Price best_bid, best_ask;      // 0 if that side is empty
    Qty   bid_size, ask_size;
    Price last_trade;
    std::int64_t now_ns;           // simulated clock
    Position position;             // this bot's inventory
    Cash   realised;               // this bot's realised P&L
    std::span<const OrderRef> my_orders;   // live orders, with queue position
};

struct Action {                    // what the bot wants to do
    enum class Kind { None, Limit, Market, Cancel } kind;
    Side side; Price price; Qty qty; OrderId cancel_id;
};

struct MarketMaker {
    static constexpr const char* name = "market maker";
    std::int64_t latency_ns = 1'000;      // its reaction time
    Action on_event(const MarketView& v); // called when the book changes
};
```

Registering one is a single line in `registry.hpp`. A first bot in ~20 lines is the
acceptance test for the API.

**Starting line-up:** market maker (quotes both sides, manages inventory), momentum
(buys strength), noise (random, provides flow), sniper (only takes quotes that are
stale relative to the last trade), and *you* (orders typed in at the keyboard).

## The latency mechanism

The one genuinely new piece. Bots cannot submit instantly, or latency means nothing:

1. The session keeps a simulated clock in nanoseconds.
2. A bot reacting at time `t` to an event has its order stamped `t + latency_ns`.
3. Orders wait in a scheduler (a small time-ordered queue) and enter the engine in
   timestamp order.

That is what makes "the fast bot eats the slow bot's edge" a measured result rather
than a claim, and it keeps sessions deterministic: same seed and same latencies give
the same session every time, which the existing event hash can verify.

## The headline experiment

**What is 10 microseconds worth?**

Run the same seeded session repeatedly, changing only one market maker's reaction time
(1µs, 10µs, 100µs, 1ms). Plot its P&L and its adverse-selection rate — how often it
gets filled right before the price moves against it — against latency. The answer is a
curve, in P&L per million orders, measured on a venue you built.

That is the quant result the whole project points at, and it is only possible because
the engine, the clock and the fills are all yours.

## Phases

Each phase ends with something runnable.

| Phase | Work | Done when |
|---|---|---|
| **P0 — look** | `ui.hpp`, static frame of the full desktop with fake numbers | A screenshot that matches the mock |
| **P1 — live book** | Feed the existing generated order stream through the engine into the view, paced for human eyes | You can watch a real book move, pause, change speed |
| **P2 — bots** | Session, scheduler, accounting, the first two bots, PARTICIPANTS window | Two bots trade each other and P&L moves |
| **P3 — sandbox** | Bot API frozen, registry, a 20-line example bot, a short "write your own bot" section in the README | Someone else can add a bot without reading the engine |
| **P4 — experiment** | Latency sweep, the P&L-vs-latency curve, written up with evidence in `results/` | The curve exists and the README leads with it |

A session hash means every phase is verifiable: same seed, same bots, same latencies →
identical trades. Test it like the engine, not like a UI.

## Risks and guards

- **Scope creep into a platform.** The guard is the table at the top. If a feature
  needs a server or a package manager, it is out.
- **The renderer eating the project.** Timebox P0 to one day. If the look is not right
  by then, simplify the look, not the engine.
- **Bots becoming the point.** They are the demo, not the research. Three good bots
  beat ten mediocre ones.
- **Determinism slipping.** Any wall-clock read inside a session breaks replay. The
  clock comes from the session, never from the OS.

## Open questions

1. **Keyboard trading:** worth it, or do bots alone make the point?
2. **How many instruments on screen at once?** One is clearer; four shows the engine
   is multi-instrument.
3. **Does the arena ship in this repo, or its own?** Same repo keeps the story
   together; a separate repo keeps this one strictly about the engine.
