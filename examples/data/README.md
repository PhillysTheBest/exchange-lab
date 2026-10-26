# Sample market data

Every file here is synthetic: made up by hand for the tests, from no real market, so it
carries no one else's licence and can be committed, under the project's own (Apache-2.0). Real data never goes in the repository; a file
you bring stays where you keep it, and its tape goes in `data/`, which git ignores
(`docs/DATA.md`).

| File | What it shows |
|---|---|
| `DEMO_2012-06-21_34200000_57600000_message_3.csv` | LOBSTER's message format: 700 orders, cancels and executions for an invented instrument, `DEMO`, with one auction trade and a halt |
| `DEMO_2012-06-21_34200000_57600000_orderbook_3.csv` | its orderbook file, three levels a side after every message; `/data add` reads it to start the book with the orders resting before the first message |
| `trades.csv` | 500 trades with a header, epoch-millisecond times and fractional quantities, as a crypto exchange's export looks |
| `databento_mbo.csv` | Databento's MBO schema as its CSV export writes it: a clear, adds, a modify, a partial cancel, and two trades each followed by the fills and cancels Databento sends for the resting orders |
| `demo_incremental_book_L2_2024-01-02_DEMO.csv` | Tardis.dev's `incremental_book_L2` file: a snapshot, level updates, and a second snapshot part-way, as when the exchange connection starts again; `/data add` turns that into a reset of the book |
| `demo_trades_2024-01-02_DEMO.csv` | its `trades` file, which `/data add` reads beside the book file and merges in by arrival time; one trade's side is `unknown` |
| `BTCUSDT-bookTicker-2024-01-02.csv` | Binance futures' best bid and ask file, as data.binance.vision names and writes it; `/data add` reads it with the trades file beside it |
| `BTCUSDT-trades-2024-01-02.csv` | that trades file, with its header; `is_buyer_maker` gives the aggressor's side |
| `ETHUSDT-trades-2025-01-02.csv` | a Binance spot trades file: no header, and times in microseconds, as spot files are from 2025 |
| `coinbase_feed.jsonl` | a capture of Coinbase Exchange's `level2_batch` and `matches` channels, one JSON message a line: a snapshot with no time, level updates, matches whose side is the resting order's, a heartbeat and a `last_match` from before the capture |
| `XBTUSD.csv` | Kraken's downloadable trade history for a pair: `timestamp,price,volume`, no header, seconds since the epoch, and no side |
| `01022024.NASDAQ_ITCH50` | Nasdaq TotalView-ITCH 5.0, binary, named as Nasdaq names a day's file: two stocks, and for `DEMO` every order message (adds with and without an MPID, executions at the order's price and at another, one not printable, a partial cancel, a replace, a delete), a hidden execution and a cross; `/data add` needs `symbol=DEMO` |
| `quotes_and_trades.csv` | best bid and ask changes and trades in one file, `;`-separated, ISO times and no header: `/data add` needs `columns=time,price,qty,side,bid,bid_qty,ask,ask_qty` |

`tests/test_data.cpp` holds the LOBSTER sample to its orderbook file after every message:
the book a tape rebuilds must be the book LOBSTER's own file says it was.
