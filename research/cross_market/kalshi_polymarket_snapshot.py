#!/usr/bin/env python3
"""research/cross_market/kalshi_polymarket_snapshot.py — Stage 6, Part C.

Offline research tool, deliberately outside include/, pipeline/, strategy/, live/: this
never runs anywhere near the trading hot path (see the plan's Part C instruction — "Do
not contaminate the low-latency execution infrastructure with exchange-specific
research code"). It answers exactly one narrow question, using REAL public data pulled
from both exchanges at call time: for a given real-world event, how far apart are
Kalshi's and Polymarket's CURRENT implied probabilities for the same outcome, right now.

This is NOT a lead-lag study. A single snapshot in time cannot show whether one
platform's price move PRECEDES the other's — that needs a time series of paired
observations across many independent events and a proper lag-correlation test. This
script is the reusable single-snapshot primitive such a study would be built on top of,
not the study itself. See docs/CROSS_MARKET_RESEARCH.md for why a full historical panel
wasn't collected this pass, for the properly-powered peer-reviewed literature this
project relies on instead (Gebele & Matthes, arXiv:2601.01706 — 100,000+ events,
2018-2025), and for one real, honestly-reported example run of this exact script.

Both endpoints used are public, unauthenticated, read-only:
  - Kalshi:     GET https://external-api.kalshi.com/trade-api/v2/markets
                (host/path confirmed against docs.kalshi.com/getting_started/
                api_environments — see live/kalshi_rest.hpp's own SCHEMA STATUS
                discipline for why this project never assumes a host/path without
                checking it first)
  - Polymarket: GET https://gamma-api.polymarket.com/public-search

No API key, no order placement, no write capability anywhere in this file — every
function here can only ever read.

Example (the pairing documented in docs/CROSS_MARKET_RESEARCH.md — both venues list a
laddered/multi-outcome market on the same September 2026 FOMC rate decision):

    python kalshi_polymarket_snapshot.py \\
        --kalshi-event KXFED-26SEP \\
        --polymarket-query "fed interest rate" \\
        --polymarket-slug-contains fed-decision-in-september
"""
from __future__ import annotations

import argparse
import json
import sys
import urllib.parse
import urllib.request
from dataclasses import dataclass

KALSHI_HOST = "https://external-api.kalshi.com/trade-api/v2"
POLYMARKET_SEARCH_HOST = "https://gamma-api.polymarket.com/public-search"
USER_AGENT = "prediction-markets-research/1.0 (offline cross-market snapshot tool)"


def _get_json(url: str) -> dict:
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=15) as resp:  # nosec - read-only, fixed hosts above
        return json.loads(resp.read().decode("utf-8"))


@dataclass
class KalshiStrike:
    ticker: str
    title: str
    yes_bid: float
    yes_ask: float

    @property
    def mid(self) -> float:
        return (self.yes_bid + self.yes_ask) / 2.0


def fetch_kalshi_event(event_ticker: str) -> list[KalshiStrike]:
    """Every open market under one Kalshi EVENT (e.g. every strike of one specific
    Fed-meeting decision, event_ticker="KXFED-26SEP") — an event, not a series
    (series_ticker="KXFED" spans every meeting past and future; confirmed empirically
    against the real API, not assumed: filtering by series_ticker alone with a
    meeting-specific suffix silently returns zero rows rather than erroring, which is
    exactly the kind of "parses nothing while reporting success" failure mode
    live/kalshi_feed.hpp's own SCHEMA STATUS notes warn about). Kalshi ladders a scalar
    outcome (like a rate decision) across many single-strike binary markets within one
    event, rather than the one multi-outcome event Polymarket uses for the same kind of
    decision (see fetch_polymarket_event below) — a real structural difference between
    the two venues' market design, not just a naming difference."""
    url = f"{KALSHI_HOST}/markets?" + urllib.parse.urlencode(
        {"limit": 200, "status": "open", "event_ticker": event_ticker}
    )
    data = _get_json(url)
    return [
        KalshiStrike(
            ticker=m["ticker"],
            title=m["title"],
            yes_bid=float(m["yes_bid_dollars"]),
            yes_ask=float(m["yes_ask_dollars"]),
        )
        for m in data.get("markets", [])
    ]


@dataclass
class PolymarketOutcome:
    question: str
    probability: float  # last-traded/mid YES probability, 0..1
    liquidity_usd: float
    volume_usd: float


def fetch_polymarket_event(query: str, event_slug_contains: str) -> list[PolymarketOutcome]:
    """Searches Polymarket's public event index and returns every sub-market
    ("outcome") of the first event whose slug contains `event_slug_contains`.
    Polymarket, unlike Kalshi, models a scalar decision as one event with several
    independent yes/no sub-markets rather than a strike ladder; this script treats each
    sub-market as one comparable "outcome," the same unit Kalshi's strike ladder is
    compared against."""
    url = f"{POLYMARKET_SEARCH_HOST}?" + urllib.parse.urlencode(
        {"q": query, "events_status": "active"}
    )
    data = _get_json(url)
    for event in data.get("events", []):
        if event_slug_contains in (event.get("slug") or ""):
            out = []
            for m in event.get("markets", []):
                prices = m.get("outcomePrices")
                if not prices:
                    continue
                # Polymarket's gamma API returns this field as a JSON-encoded STRING
                # ('["0.625", "0.375"]'), not a native array — confirmed empirically
                # against this exact endpoint (a real, observed API quirk, not assumed;
                # same "check, don't guess" discipline live/kalshi_feed.hpp's own SCHEMA
                # STATUS notes apply to Kalshi's wire format). Handle both shapes rather
                # than assuming one.
                if isinstance(prices, str):
                    prices = json.loads(prices)
                out.append(
                    PolymarketOutcome(
                        question=m.get("question", ""),
                        probability=float(prices[0]),
                        liquidity_usd=float(m.get("liquidity") or 0.0),
                        volume_usd=float(m.get("volume") or 0.0),
                    )
                )
            return out
    return []


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--kalshi-event", required=True, help="Kalshi event_ticker, e.g. KXFED-26SEP")
    ap.add_argument("--polymarket-query", required=True, help="Polymarket public-search query, e.g. 'fed interest rate'")
    ap.add_argument(
        "--polymarket-slug-contains",
        required=True,
        help="substring to pick the right event out of the search results, e.g. 'fed-decision-in-september'",
    )
    args = ap.parse_args()

    kalshi = fetch_kalshi_event(args.kalshi_event)
    poly = fetch_polymarket_event(args.polymarket_query, args.polymarket_slug_contains)

    if not kalshi:
        print(f"no open Kalshi markets found for event {args.kalshi_event!r}", file=sys.stderr)
        return 1
    if not poly:
        print(f"no Polymarket event found matching slug substring {args.polymarket_slug_contains!r}", file=sys.stderr)
        return 1

    print(f"== Kalshi: {args.kalshi_event} ==")
    for k in kalshi:
        print(f"  {k.ticker:28s} mid={k.mid:.4f}  bid={k.yes_bid:.4f} ask={k.yes_ask:.4f}  {k.title}")

    print(f"\n== Polymarket: event matching slug substring {args.polymarket_slug_contains!r} ==")
    for p in poly:
        print(f"  p={p.probability:.4f}  liq=${p.liquidity_usd:,.0f}  vol=${p.volume_usd:,.0f}  {p.question}")

    print(
        "\nNOTE: this is a single point-in-time snapshot, not a time series - it "
        "cannot show whether one venue leads the other. See docs/CROSS_MARKET_RESEARCH.md."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
