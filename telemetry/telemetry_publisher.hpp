// telemetry/telemetry_publisher.hpp — Stage 6, Part E/F/G: the ONLY bridge between
// this C++ process and the Rust/Axum dashboard (dashboard/) — a plain JSON file,
// written atomically (temp file + rename, never an in-place partial write a
// concurrent reader could observe half-written), polled by the dashboard on its own
// schedule. No socket, no shared memory, no IPC library: the dashboard is a
// completely separate process (arguably a separate machine, if someone pointed it at
// a shared filesystem) that must be free to crash, restart, or simply not be running
// at all without this process ever noticing or caring — the plan's Part F
// requirement ("Telemetry must never block the Brain thread... the trading engine
// must never wait for the dashboard") taken to its logical conclusion: not just
// non-blocking, but with NO channel back to this process at all.
//
// Called from tools/kalshi_pipeline.cpp's own main thread — already polling every
// 500ms to print stats (see that file) — not a new 4th thread. Publishing a small
// JSON blob every ~500ms-1s is exactly the kind of low-frequency, non-hot-path
// bookkeeping a main/supervisor thread is for.
#pragma once

#include "telemetry/telemetry.hpp"

#include <cstdio>
#include <string>

namespace px::telemetry {

namespace detail {

[[nodiscard]] inline std::string json_escape(const char* s) {
    std::string out;
    for (const char* p = s; *p; ++p) {
        if (*p == '"' || *p == '\\') out.push_back('\\');
        if (*p == '\0') break;
        out.push_back(*p);
    }
    return out;
}

[[nodiscard]] inline const char* order_kind_str(OrderEventKind k) noexcept {
    switch (k) {
        case OrderEventKind::Submit: return "submit";
        case OrderEventKind::Ack: return "ack";
        case OrderEventKind::Fill: return "fill";
        case OrderEventKind::Cancel: return "cancel";
        case OrderEventKind::Reject: return "reject";
        case OrderEventKind::RiskBlocked: return "risk_blocked";
    }
    return "unknown";
}

} // namespace detail

// Serializes the current (racily-read, intentionally so — see telemetry.hpp) state
// of `t` to JSON and writes it to `path` via a temp-file-then-rename, so a reader
// (the dashboard's Rust process) polling `path` on its own schedule never observes a
// partially-written file — `rename()` (Windows: `MoveFileExW` with
// `MOVEFILE_REPLACE_EXISTING`, which std::rename uses on this toolchain) is atomic
// at the filesystem level on both POSIX and Windows for a same-volume same-directory
// rename, the only kind this ever does: a reader that DOES manage to open `path`
// either gets the complete old file or the complete new one, never a half-written one.
//
// WINDOWS CAVEAT, found empirically (not theorized) while building the dashboard
// (Stage 6, Part E/F): the rename call ITSELF can transiently fail (return false, no
// file written this tick) if the dashboard reader's own file handle is open at that
// exact instant — Windows, unlike POSIX, can refuse to replace a file over an open
// read handle depending on the sharing mode the reader used (`dashboard/src/main.rs`
// reads via `tokio::fs::read_to_string`, i.e. `std::fs::File::open`'s default sharing,
// which does not request `FILE_SHARE_DELETE`). Deliberately NOT worked around by
// adding Windows-specific sharing flags to the Rust reader: this function already
// returns `false` on failure, `tools/kalshi_pipeline.cpp`'s caller already logs it and
// tries again next 500ms tick, and the dashboard's own read failure (`px-dashboard`'s
// `/api/snapshot` returning 503) is itself non-fatal and self-heals the same way, one
// poll interval later — see `static/index.html`'s explicit "show last-known-good data,
// mark it stale" handling. A guaranteed-first-try rename isn't worth Windows-only
// conditional code in a cross-platform-intended reader for a race window this narrow
// and this self-correcting.
[[nodiscard]] inline bool publish_snapshot(const TelemetryState& t, const std::string& path) {
    const std::string tmp_path = path + ".tmp";
    FILE* f = std::fopen(tmp_path.c_str(), "wb");
    if (!f) return false;

    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"connected\": %s,\n", t.connected() ? "true" : "false");
    std::fprintf(f, "  \"mode\": \"%s\",\n", t.mode() == Mode::Live ? "live" : "dry_run");
    std::fprintf(f, "  \"conn_status\": \"%s\",\n",
                  t.conn_status() == ConnStatus::Connected  ? "connected"
                  : t.conn_status() == ConnStatus::Connecting ? "connecting"
                                                               : "disconnected");
    std::fprintf(f, "  \"start_ts_ms\": %lld,\n", static_cast<long long>(t.start_ts_ms()));
    std::fprintf(f, "  \"reconnect_count\": %u,\n", t.reconnect_count());
    std::fprintf(f, "  \"active_markets\": %u,\n", t.active_markets());

    std::fprintf(f, "  \"events_received\": %llu,\n", static_cast<unsigned long long>(t.events_received()));
    std::fprintf(f, "  \"events_processed\": %llu,\n", static_cast<unsigned long long>(t.events_processed()));
    std::fprintf(f, "  \"strategy_decisions\": %llu,\n", static_cast<unsigned long long>(t.strategy_decisions()));
    std::fprintf(f, "  \"orders_submitted\": %llu,\n", static_cast<unsigned long long>(t.orders_submitted()));
    std::fprintf(f, "  \"orders_acked\": %llu,\n", static_cast<unsigned long long>(t.orders_acked()));
    std::fprintf(f, "  \"orders_filled\": %llu,\n", static_cast<unsigned long long>(t.orders_filled()));
    std::fprintf(f, "  \"orders_cancelled\": %llu,\n", static_cast<unsigned long long>(t.orders_cancelled()));
    std::fprintf(f, "  \"orders_rejected\": %llu,\n", static_cast<unsigned long long>(t.orders_rejected()));
    std::fprintf(f, "  \"risk_rejections\": %llu,\n", static_cast<unsigned long long>(t.risk_rejections()));
    std::fprintf(f, "  \"fees_cents\": %lld,\n", static_cast<long long>(t.fees_cents()));

    // ---- risk limits (Stage 6A): static config + the one live figure (rate-limit
    // token headroom) Brain samples on its own thread -- see telemetry.hpp ----
    std::fprintf(f, "  \"risk\": {\"max_exposure_cents\": %lld, \"fat_finger_min\": %d, \"fat_finger_max\": %d, "
                     "\"rate_limit_tokens\": %lld, \"rate_limit_capacity\": %lld},\n",
                  static_cast<long long>(t.max_exposure_cents()), t.fat_finger_min(), t.fat_finger_max(),
                  static_cast<long long>(t.rate_limit_tokens()), static_cast<long long>(t.rate_limit_capacity()));

    std::int64_t realized_total = 0, unrealized_total = 0, exposure_total = 0;
    std::fprintf(f, "  \"markets\": [\n");
    bool first_market = true;
    for (const auto& m : t.markets()) {
        if (!m.in_use.load(std::memory_order_relaxed)) continue;
        const auto realized = m.realized_pnl_cents.load(std::memory_order_relaxed);
        const auto unrealized = m.unrealized_pnl_cents.load(std::memory_order_relaxed);
        const auto exposure = m.exposure_cents.load(std::memory_order_relaxed);
        realized_total += realized;
        unrealized_total += unrealized;
        exposure_total += exposure;
        if (!first_market) std::fprintf(f, ",\n");
        first_market = false;
        std::fprintf(f,
                      "    {\"market\": %u, \"position\": %lld, \"avg_entry_cents\": %d, "
                      "\"yes_bid_cents\": %d, \"yes_ask_cents\": %d, \"realized_pnl_cents\": %lld, "
                      "\"unrealized_pnl_cents\": %lld, \"exposure_cents\": %lld, \"status\": %u, ",
                      m.market, static_cast<long long>(m.position.load(std::memory_order_relaxed)),
                      m.avg_entry_cents.load(std::memory_order_relaxed), m.yes_bid_cents.load(std::memory_order_relaxed),
                      m.yes_ask_cents.load(std::memory_order_relaxed), static_cast<long long>(realized),
                      static_cast<long long>(unrealized), static_cast<long long>(exposure),
                      m.status.load(std::memory_order_relaxed));

        // Depth (Stage 6A): nearest-price-first, bounded to whatever count Brain last
        // published (0..kDepthLevels) -- an empty array is a legitimate "no resting
        // interest observed on this side," not an error.
        const auto bid_n = m.bid_depth_count.load(std::memory_order_relaxed);
        std::fprintf(f, "\"bid_depth\": [");
        for (std::uint8_t i = 0; i < bid_n; ++i) {
            std::fprintf(f, "%s{\"price\": %d, \"qty\": %u}", i == 0 ? "" : ", ",
                          m.bid_depth_price[i].load(std::memory_order_relaxed),
                          m.bid_depth_qty[i].load(std::memory_order_relaxed));
        }
        const auto ask_n = m.ask_depth_count.load(std::memory_order_relaxed);
        std::fprintf(f, "], \"ask_depth\": [");
        for (std::uint8_t i = 0; i < ask_n; ++i) {
            std::fprintf(f, "%s{\"price\": %d, \"qty\": %u}", i == 0 ? "" : ", ",
                          m.ask_depth_price[i].load(std::memory_order_relaxed),
                          m.ask_depth_qty[i].load(std::memory_order_relaxed));
        }
        std::fprintf(f, "]}");
    }
    std::fprintf(f, "\n  ],\n");
    std::fprintf(f, "  \"pnl_realized_cents\": %lld,\n", static_cast<long long>(realized_total));
    std::fprintf(f, "  \"pnl_unrealized_cents\": %lld,\n", static_cast<long long>(unrealized_total));
    std::fprintf(f, "  \"pnl_net_cents\": %lld,\n",
                  static_cast<long long>(realized_total + unrealized_total - t.fees_cents()));
    std::fprintf(f, "  \"exposure_cents\": %lld,\n", static_cast<long long>(exposure_total));

    std::fprintf(f, "  \"recent_orders\": [\n");
    const auto orders = t.recent_orders();
    for (std::size_t i = 0; i < orders.size(); ++i) {
        const auto& o = orders[i];
        std::fprintf(f,
                      "    {\"ts_ms\": %lld, \"market\": %u, \"id\": %llu, \"kind\": \"%s\", \"side\": \"%s\", "
                      "\"price\": %d, \"qty\": %u}%s\n",
                      static_cast<long long>(o.ts_ms), o.market, static_cast<unsigned long long>(o.id),
                      detail::order_kind_str(o.kind), o.side == Side::Buy ? "buy" : "sell", o.price, o.qty,
                      i + 1 < orders.size() ? "," : "");
    }
    std::fprintf(f, "  ],\n");

    std::fprintf(f, "  \"recent_trades\": [\n");
    const auto trades = t.recent_trades();
    for (std::size_t i = 0; i < trades.size(); ++i) {
        const auto& tr = trades[i];
        std::fprintf(f, "    {\"ts_ms\": %lld, \"market\": %u, \"side\": \"%s\", \"price\": %d, \"qty\": %u}%s\n",
                      static_cast<long long>(tr.ts_ms), tr.market, tr.side == Side::Buy ? "buy" : "sell", tr.price,
                      tr.qty, i + 1 < trades.size() ? "," : "");
    }
    std::fprintf(f, "  ],\n");

    std::fprintf(f, "  \"pnl_history\": [\n");
    const auto pnl_hist = t.pnl_history();
    for (std::size_t i = 0; i < pnl_hist.size(); ++i) {
        const auto& p = pnl_hist[i];
        std::fprintf(f, "    {\"ts_ms\": %lld, \"net_pnl_cents\": %lld}%s\n", static_cast<long long>(p.ts_ms),
                      static_cast<long long>(p.net_pnl_cents), i + 1 < pnl_hist.size() ? "," : "");
    }
    std::fprintf(f, "  ],\n");

    std::fprintf(f, "  \"recent_decisions\": [\n");
    const auto decisions = t.recent_decisions();
    for (std::size_t i = 0; i < decisions.size(); ++i) {
        const auto& d = decisions[i];
        std::fprintf(f,
                      "    {\"ts_ms\": %lld, \"market\": %u, \"strategy\": \"%s\", \"price\": %d, \"qty\": %u, "
                      "\"reason\": \"%s\"}%s\n",
                      static_cast<long long>(d.ts_ms), d.market, detail::json_escape(d.strategy_name).c_str(), d.price,
                      d.qty, detail::json_escape(d.reason).c_str(), i + 1 < decisions.size() ? "," : "");
    }
    std::fprintf(f, "  ]\n");
    std::fprintf(f, "}\n");

    std::fclose(f);
    return std::rename(tmp_path.c_str(), path.c_str()) == 0;
}

} // namespace px::telemetry
