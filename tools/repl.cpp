// tools/repl.cpp — drive the matching engine by hand and watch its events.
//
//   > add ACME                  register an instrument (tick $0.01; `add SYM 0.05` for another)
//   > sell ACME 100 10.02       GTC limit;  append `ioc` for immediate-or-cancel
//   > buy ACME 40 10.05 as 7    `as N` submits as participant N (default 1)
//   > mbuy ACME 25              market order (mbuy / msell)
//   > cancel 3                  cancel order #3 (only its owner may)
//   > book ACME                 show the book, best prices nearest the middle line
//
// Prices are typed and shown in dollars; internally they are int64 fixed point
// with 4 decimal places ($1.00 == 10000). Order ids are assigned by the REPL.
#include "px/matching_engine.hpp"

#include <cstdio>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define PX_ISATTY _isatty
#define PX_FILENO _fileno
#else
#include <unistd.h>
#define PX_ISATTY isatty
#define PX_FILENO fileno
#endif

using namespace px;

namespace {

// "10.05" -> 100500. Rejects more than 4 decimal places rather than rounding.
bool parse_price(const std::string& s, Price& out) {
    Price whole = 0, frac = 0;
    int frac_digits = 0;
    bool seen_dot = false, any_digit = false;
    for (const char c : s) {
        if (c == '.' && !seen_dot) {
            seen_dot = true;
        } else if (c >= '0' && c <= '9') {
            any_digit = true;
            if (seen_dot) {
                if (++frac_digits > 4) return false;
                frac = frac * 10 + (c - '0');
            } else {
                whole = whole * 10 + (c - '0');
                if (whole > kMaxPrice / kPriceScale) return false;
            }
        } else {
            return false;
        }
    }
    while (frac_digits++ < 4) frac *= 10;
    out = whole * kPriceScale + frac;
    return any_digit;
}

std::string fmt_price(Price p) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld.%04lld", static_cast<long long>(p / kPriceScale),
                  static_cast<long long>(p % kPriceScale));
    std::string s = buf;
    while (s.size() > 3 && s.back() == '0' && s[s.size() - 3] != '.') s.pop_back(); // keep >= 2 decimals
    return s;
}

const char* reject_name(std::uint8_t r) {
    switch (static_cast<RejectReason>(r)) {
        case RejectReason::UnknownInstrument: return "unknown instrument";
        case RejectReason::InvalidQty: return "invalid quantity";
        case RejectReason::InvalidPrice: return "invalid price (<= 0, too large, or off the tick grid)";
        case RejectReason::InvalidOrderId: return "invalid order id";
        case RejectReason::DuplicateOrderId: return "duplicate order id";
        case RejectReason::BookFull: return "book full";
        case RejectReason::UnknownOrder: return "unknown order (or not yours)";
        case RejectReason::None: break;
    }
    return "?";
}

const char* cancel_name(std::uint8_t r) {
    switch (static_cast<CancelReason>(r)) {
        case CancelReason::User: return "cancelled";
        case CancelReason::IocRemainder: return "unfilled remainder cancelled (IOC/market)";
        case CancelReason::SelfTrade: return "cancelled to prevent a self-trade";
        case CancelReason::None: break;
    }
    return "?";
}

void print_event(void*, const ExecEvent& e) {
    const char* side = e.side == Side::Buy ? "buy" : "sell";
    switch (e.type) {
        case EventType::Ack:
            std::printf("  ack     #%llu %s %u%s%s\n", static_cast<unsigned long long>(e.order_id), side, e.qty,
                        e.price ? " @ " : " at market", e.price ? fmt_price(e.price).c_str() : "");
            break;
        case EventType::Trade:
            std::printf("  trade   #%llu x #%llu: %u @ %s  (#%llu has %u left, #%llu has %u left)\n",
                        static_cast<unsigned long long>(e.order_id), static_cast<unsigned long long>(e.maker_id), e.qty,
                        fmt_price(e.price).c_str(), static_cast<unsigned long long>(e.order_id), e.leaves,
                        static_cast<unsigned long long>(e.maker_id), e.maker_leaves);
            break;
        case EventType::Cancelled:
            std::printf("  cancel  #%llu: %u %s\n", static_cast<unsigned long long>(e.order_id), e.qty,
                        cancel_name(e.reason));
            break;
        case EventType::Rejected:
            std::printf("  reject  #%llu: %s\n", static_cast<unsigned long long>(e.order_id), reject_name(e.reason));
            break;
        case EventType::EndOfStream:
            break;
    }
}

void print_book(const MatchingEngine& eng, InstrumentId id, std::uint32_t depth) {
    const std::uint32_t asks = std::min(depth, eng.depth(id, Side::Sell));
    const std::uint32_t bids = std::min(depth, eng.depth(id, Side::Buy));
    std::printf("  %s  (tick %s)\n", std::string(eng.symbol(id)).c_str(), fmt_price(eng.tick_size(id)).c_str());
    for (std::uint32_t r = asks; r-- > 0;) {
        const LevelView l = eng.level(id, Side::Sell, r);
        std::printf("    ask %12s  %8llu  (%u order%s)\n", fmt_price(l.price).c_str(),
                    static_cast<unsigned long long>(l.qty), l.orders, l.orders == 1 ? "" : "s");
    }
    std::printf("    ------------------------------------\n");
    for (std::uint32_t r = 0; r < bids; ++r) {
        const LevelView l = eng.level(id, Side::Buy, r);
        std::printf("    bid %12s  %8llu  (%u order%s)\n", fmt_price(l.price).c_str(),
                    static_cast<unsigned long long>(l.qty), l.orders, l.orders == 1 ? "" : "s");
    }
}

void help() {
    std::printf("commands:\n"
                "  add SYM [TICK]              register an instrument (default tick 0.01)\n"
                "  buy|sell SYM QTY PRICE [ioc] [as OWNER]\n"
                "  mbuy|msell SYM QTY [as OWNER]  market order\n"
                "  cancel ID [as OWNER]\n"
                "  book SYM [DEPTH]\n"
                "  help, quit\n");
}

} // namespace

int main() {
    auto eng = std::make_unique<MatchingEngine>();
    const EventSink sink(nullptr, &print_event);
    const bool interactive = PX_ISATTY(PX_FILENO(stdin)) != 0;
    OrderId next_id = 1;
    if (interactive) std::printf("matching engine REPL — type `help`\n");

    const auto find_instrument = [&](const std::string& sym) -> InstrumentId {
        for (std::size_t i = 0; i < eng->instrument_count(); ++i) {
            if (eng->symbol(static_cast<InstrumentId>(i)) == sym) return static_cast<InstrumentId>(i);
        }
        return kInvalidInstrument;
    };

    std::string line;
    while (true) {
        if (interactive) std::printf("> ");
        std::fflush(stdout);
        if (!std::getline(std::cin, line)) break;
        std::istringstream in(line);
        std::vector<std::string> w;
        for (std::string t; in >> t;) w.push_back(t);
        if (w.empty() || w[0][0] == '#') continue;
        if (!interactive) std::printf("> %s\n", line.c_str()); // echo scripted input

        // Optional trailing "as OWNER".
        OwnerId owner = 1;
        if (w.size() >= 2 && w[w.size() - 2] == "as") {
            owner = static_cast<OwnerId>(std::stoul(w.back()));
            w.resize(w.size() - 2);
        }
        const std::string& cmd = w[0];
        try {
            if (cmd == "quit" || cmd == "exit") {
                break;
            } else if (cmd == "help") {
                help();
            } else if (cmd == "add" && (w.size() == 2 || w.size() == 3)) {
                Price tick = kPriceScale / 100;
                if (w.size() == 3 && !parse_price(w[2], tick)) throw std::invalid_argument("bad tick");
                if (find_instrument(w[1]) != kInvalidInstrument) throw std::invalid_argument("already exists");
                const InstrumentId id = eng->add_instrument(w[1], tick);
                if (id == kInvalidInstrument) throw std::invalid_argument("cannot add instrument");
                std::printf("  added %s (tick %s)\n", w[1].c_str(), fmt_price(tick).c_str());
            } else if ((cmd == "buy" || cmd == "sell") && (w.size() == 4 || w.size() == 5)) {
                Price px = 0;
                if (!parse_price(w[3], px)) throw std::invalid_argument("bad price");
                const Tif tif = w.size() == 5 && w[4] == "ioc" ? Tif::IOC : Tif::GTC;
                if (w.size() == 5 && w[4] != "ioc") throw std::invalid_argument("expected `ioc`");
                eng->process(limit_order(next_id++, find_instrument(w[1]), owner,
                                         cmd == "buy" ? Side::Buy : Side::Sell, px,
                                         static_cast<Qty>(std::stoul(w[2])), tif),
                             sink);
            } else if ((cmd == "mbuy" || cmd == "msell") && w.size() == 3) {
                eng->process(market_order(next_id++, find_instrument(w[1]), owner,
                                          cmd == "mbuy" ? Side::Buy : Side::Sell, static_cast<Qty>(std::stoul(w[2]))),
                             sink);
            } else if (cmd == "cancel" && w.size() == 2) {
                eng->process(cancel_order(std::stoull(w[1]), 0, owner), sink);
            } else if (cmd == "book" && (w.size() == 2 || w.size() == 3)) {
                const InstrumentId id = find_instrument(w[1]);
                if (id == kInvalidInstrument) throw std::invalid_argument("unknown instrument");
                print_book(*eng, id, w.size() == 3 ? static_cast<std::uint32_t>(std::stoul(w[2])) : 10);
            } else {
                std::printf("  ? (type `help`)\n");
            }
        } catch (const std::exception& ex) {
            std::printf("  error: %s\n", ex.what());
        }
    }
    return 0;
}
