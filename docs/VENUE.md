# The venue: what stands around the engine

The matching engine decides who trades with whom. A real exchange needs a good deal more
around it before an order reaches the book and after an event leaves it: something that puts
many senders into one order, something that stops an obviously wrong order, a record that
lets the book be rebuilt after a crash, a second copy ready to take over, and a public feed
that tells the market what happened without saying who did it. `venue/` is that layer.

It is a teaching model of those stages, built so each one can be read in a single header and
checked by a test. It is not a production exchange, and it says where it falls short.

```
  session messages
        |
        v
  [ Sequencer ] -> [ RiskGate ] -> [ journal ] -> [ MatchingEngine ] -> [ Publisher ]
   one order        sane orders     on disk,        unchanged:          |-> private reports
   for everyone     only            before          process() only      '-> public feed
                                    matching             |
                                                         '-> standby follows the journal
```

`venue/venue.hpp` joins the stages in that order around one engine. Everything in `venue/` is
header-only, allocates freely, does its file I/O off the engine's timed path, and uses the
engine through `process()` alone. **The engine gained no API for any of it**, so no pinned
hash or fingerprint moved.

## Try it

```bash
make test                         # includes test_venue, which checks every stage below
make test-net                     # the loopback demonstration: two clients, one venue, every step checked
make repl && build/release/repl < tools/repl_demo.txt   # the hand-driven book, ending with two amends
build/release/bench_matching_latency --stages           # the per-stage time stamps
```

In the app, `/book` is the same hand-driven book, so `amend ID PRICE QTY as OWNER` works there
too; type `help` inside it for the full list.

## The stages

### Sequencer: one order for everyone (`venue/sequencer.hpp`)

The engine takes requests from one caller, so on its own it never has to decide which of two
orders came first. A venue with many connections does, and the sequencer is the single place
where that is decided. Everything downstream (the risk gate, the journal, the engine, a
standby) sees one numbered stream and nothing else, which is why they can all agree.

- **Each session numbers its own messages from 1.** A repeated number is a resend and is
  dropped, so a client that retries after a timeout cannot enter the same order twice. A
  skipped number is refused, so the client knows to resend what was lost.
- **Fairness is round-robin.** Each pass takes at most one message from each session, and the
  session a pass starts with moves on each time, so no sender can starve another by sending
  faster. That is a stated rule, not a claim about arrival time.
- `MergeSequencer` is the threaded form: one lock-free ring per session (the engine's own
  `SpscRing`) and one thread putting them in line.

### Risk gate and kill switch (`venue/risk.hpp`)

The engine checks that an order is well formed. It does not ask whether the order is sane. The
gate asks that first, per member:

- **size and value:** one order may not exceed a quantity or a notional value;
- **price collar:** a limit price may not be too far from the last trade, and a market order
  becomes a limit order at the collar, so it cannot sweep the book to any price;
- **open orders:** a cap on how many orders one member may have resting;
- **throttle:** a cap on one member's messages per window, counted in venue messages rather
  than wall-clock time, so a replay of the journal makes the same decisions;
- **kill switch:** blocks a member and hands back a cancel for every order it has resting.

Cancels are never refused, because a member must always be able to get out. An amend is
checked like a new order at its new price and size. Everything in the gate is integer and
reads no clock.

### Journal, snapshot and crash recovery (`venue/journal.hpp`, `book_state.hpp`, `recovery.hpp`)

Because the engine gives the same output for the same input, it can be rebuilt after a crash
from its input alone. The journal is that input, written down: every request the engine was
given, numbered and checksummed, written before the engine sees it. An order that could not be
recorded is not allowed to trade.

- **A torn tail is dropped, never replayed.** A crash can leave half a record at the end of
  the file; the reader stops at the first record that is short or fails its checksum.
- **A snapshot is the book at rest.** The venue keeps its own list of resting orders, built
  from the event stream the way a strategy's book mirror is, because the engine exposes price
  levels and not the orders in them. Restoring sends those orders into a fresh engine in their
  original time order; a book at rest is never crossed, so nothing trades and every queue comes
  back as it was.
- **Recovery is restore, then replay.** The test cuts a run at an arbitrary request, recovers
  from the snapshot and the journal, finishes the run and compares the hash of the whole event
  stream with a run that was never interrupted. They match.
- **Hot standby.** A second engine follows the journal as it grows and is promoted simply by
  being given requests. If a record goes missing, the standby stops rather than carry on with
  a book that has quietly diverged; that case is a planted defect in the test.

Where it falls short of a real venue: the journal flushes through the C library and does not
`fsync`, so it survives the process dying, not the machine losing power, and it is not
replicated to another machine before an order is acknowledged. The risk gate's own memory
(throttle counts, the kill list, the last trade price) is not restored after a restart.

### Public feed and private reports, with gap recovery (`venue/feed.hpp`)

The engine emits one stream that names both sides of every trade. A venue never publishes
that. The publisher splits it in two:

- **private reports** go to one member each, about that member's own orders only. A fill says
  your order, the price and the size, and never who was on the other side;
- **the public feed** is anonymous and order by order, in the style of Nasdaq's ITCH: an order
  comes to rest, is executed, is deleted, or is reduced in place. Orders are named by a
  reference number the feed makes up, and the wire format has no field that could carry an
  owner.

The feed is numbered, so a listener that misses a message knows it. `FeedBook` is such a
listener: it rebuilds the book from the public feed alone, refuses a message that does not
follow the last, and catches up from a snapshot. The test drops messages on purpose and checks
the rebuilt book against the engine's own.

### Order amend (in the engine)

Amend is the one feature here that lives inside the engine, because only the engine knows an
order's place in its queue. Making an order smaller at the same price changes it in place and
**keeps its time priority**; changing its price or making it larger sends it to **the back of
the queue** under the same id, where it can trade like any new order. If the amended order
could not be given a place to rest, the request is refused whole and the order is left as it
was. The naive reference engine implements the same rule, and the differential fuzz test
compares the two with amends in the stream. Adding amend moved no pinned hash.

### Per-stage time stamps (`sim/pipeline.hpp`, `bench_matching_latency --stages`)

The engine's published measurement is service time: from taking an order off the inbound ring
to writing its last event. `--stages` adds a stamp when the gateway offers the order, so the
benchmark can also report how long each order waited for the matcher, and the two together.
That extra report is not part of the gate and is not service time. Stamping changes nothing
that is matched: the test checks the event hash with and without it.

### Instruments sharded across matcher threads (`venue/shards.hpp`)

One engine on one thread needs no locks because nothing is shared. To use more cores, a venue
runs several engines, gives each a fixed set of instruments, and puts a router in front.
Orders in different instruments never interact, so nothing has to be coordinated between
shards, and each shard has its own pools, so one busy instrument cannot use up the room
another needs.

What stays true: each shard's event stream depends only on the requests routed to it, so
thread timing cannot change it. The test checks every shard against a single-threaded run of
its own requests, and every instrument's final book against one unsharded engine's. What
changes: sequence numbers are per shard, so there is no longer one global order of events, as
on a real venue that publishes one feed channel per shard.

### Order-entry protocol and the loopback demonstration (`venue/wire.hpp`, `tools/netdemo.cpp`)

`venue/wire.hpp` is a small binary order-entry protocol in the style of an exchange's native
one (Nasdaq's OUCH is the well-known example): fixed little-endian fields and a length in front
of every message, because TCP delivers bytes, not messages. It does no I/O, and its frame
reader is tested by feeding it one byte at a time.

`tools/netdemo.cpp` is **the one program in the repository with a socket in it**, chosen on
2026-11-15 as a deliberate exception to the no-network rule. It is a demonstration, not an
experiment: it listens on the loopback address only, connects two clients from the same
process and checks each step of a short script (logon, an order and its fill, private reports
to both sides with no names attached, a resend dropped, a skipped number refused, a message
split in two, cancel on disconnect, and a reconnect that resumes where the session left off).
It exits 0 only if every step held, so `make test-net` is a test. It is not part of `make all`
or anything that produces evidence. It uses POSIX sockets; on Windows it says so and exits 0.

## What the tests check

`tests/test_venue.cpp` runs in `make test`. It covers the journal's round trip and its torn
tail; crash recovery against an uninterrupted run; that a snapshot is the book; a standby
taking over, and stopping on a lost record; the public feed and private reports, with messages
dropped on purpose; every risk check and the kill switch; the sequencer's resend, gap and
fairness rules; that stage stamps change nothing; shards against single-threaded runs; the
wire format byte by byte; and amend through the whole venue.

## What it does not change

- **The engine's rules hold.** It stays single-threaded, allocates nothing after
  construction, and is called only through `process()`.
- **The arena is untouched.** Sessions, suites and every published result are produced exactly
  as before, and nothing in `arena/` calls the venue.
- **One socket, on the loopback address, in one demonstration.** Every program that produces
  evidence still runs in one process with no network.
