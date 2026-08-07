// tools/repl.cpp — interactive command-line front end for MatchingEngine.
//
// Not on any hot path (it's a REPL — line-buffered stdin), so it's free to use
// std::string/std::cin/exceptions throughout despite the rest of the codebase
// avoiding them; that boundary is deliberate, not an oversight.
#include "px/matching_engine.hpp"

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>

using namespace px;

namespace {

void print_help() {
    std::cout <<
        "Commands:\n"
        "  market create <ticker>                    create a market (PreOpen)\n"
        "  market open|halt|resume <id>               lifecycle transitions\n"
        "  market resolve <id> yes|no                 terminal settlement\n"
        "  trader create <cash_cents>                 create a trader account\n"
        "  buy  <mkt> <trader> yes|no <price> <qty> [ioc]   LIMIT buy\n"
        "  sell <mkt> <trader> yes|no <price> <qty> [ioc]   LIMIT sell\n"
        "  mbuy  <mkt> <trader> yes|no <qty>          MARKET buy (always IOC)\n"
        "  msell <mkt> <trader> yes|no <qty>          MARKET sell (always IOC)\n"
        "  cancel <mkt> <order_id>\n"
        "  book <mkt>                                 dump best bid/ask + full ladder\n"
        "  trader <id>                                dump cash + positions\n"
        "  trades                                     trades from the last submit\n"
        "  help\n"
        "  quit\n";
}

const char* reject_str(RejectReason r) {
    switch (r) {
        case RejectReason::None: return "ok";
        case RejectReason::MarketNotOpen: return "market not open (or unknown market)";
        case RejectReason::InvalidPrice: return "invalid price (must be 1..99)";
        case RejectReason::InvalidQty: return "invalid qty (must be > 0)";
        case RejectReason::UnknownOrder: return "unknown order/trader";
        case RejectReason::DuplicateOrderId: return "duplicate order id";
        case RejectReason::InsufficientCash: return "insufficient cash";
        case RejectReason::InsufficientPosition: return "insufficient position";
        case RejectReason::PoolExhausted: return "order pool exhausted";
    }
    return "?";
}

Contract parse_contract(const std::string& s) {
    return (s == "no" || s == "NO" || s == "No") ? Contract::No : Contract::Yes;
}

void print_submit_result(const MatchingEngine::SubmitReport& r) {
    std::cout << "  -> order_id=" << r.order_id << " filled=" << r.filled_qty
               << " rested=" << r.rested_qty << " status=" << reject_str(r.reject) << "\n";
}

void dump_book(MatchingEngine& eng, MarketId m) {
    Market* mkt = eng.market(m);
    if (!mkt) { std::cout << "  no such market\n"; return; }
    Price bb = 0, ba = 0;
    const bool have_bb = mkt->book().best_bid(bb);
    const bool have_ba = mkt->book().best_ask(ba);
    std::cout << "  best bid: " << (have_bb ? std::to_string(bb) : std::string("-"))
               << "   best ask: " << (have_ba ? std::to_string(ba) : std::string("-")) << "\n";
    std::cout << "  ladder (price: qty), asks descending then bids descending:\n";
    for (Price p = kMaxPrice; p >= kMinPrice; --p) {
        const auto& lvl = mkt->book().level(p);
        if (lvl.total_qty > 0) {
            const char* side = (have_ba && p >= ba) ? "ASK" : "BID";
            std::cout << "    " << side << " " << p << ": " << lvl.total_qty << "\n";
        }
    }
}

void dump_trader(MatchingEngine& eng, TraderId id) {
    Trader* t = eng.trader(id);
    if (!t) { std::cout << "  no such trader\n"; return; }
    std::cout << "  trader " << id << ": cash=" << t->cash() << " cents\n";
}

void dump_trades(MatchingEngine& eng) {
    auto trades = eng.last_trades();
    if (trades.empty()) { std::cout << "  (no trades from the last submit)\n"; return; }
    for (const auto& t : trades) {
        std::cout << "  trade seq=" << t.seq << " mkt=" << t.market_id
                   << " taker=" << t.taker_id << " maker=" << t.maker_id
                   << " price=" << t.price << " qty=" << t.qty << "\n";
    }
}

} // namespace

int main() {
    MatchingEngine eng;
    std::cout << "prediction-markets REPL. Type 'help' for commands, 'quit' to exit.\n";

    std::string line;
    while (true) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;
        if (cmd.empty()) continue;

        if (cmd == "quit" || cmd == "exit") {
            break;
        } else if (cmd == "help") {
            print_help();
        } else if (cmd == "market") {
            std::string sub;
            iss >> sub;
            if (sub == "create") {
                std::string ticker;
                iss >> ticker;
                const MarketId id = eng.create_market(ticker);
                std::cout << "  created market " << id << " (" << ticker << "), status=PreOpen\n";
            } else if (sub == "open" || sub == "halt" || sub == "resume") {
                MarketId id;
                iss >> id;
                bool ok = (sub == "open") ? eng.open_market(id)
                        : (sub == "halt") ? eng.halt_market(id)
                                           : eng.resume_market(id);
                std::cout << (ok ? "  ok\n" : "  rejected: invalid market id or transition\n");
            } else if (sub == "resolve") {
                MarketId id;
                std::string outcome;
                iss >> id >> outcome;
                const Settlement s = (outcome == "yes") ? Settlement::Yes
                                    : (outcome == "no")  ? Settlement::No
                                                          : Settlement::Unresolved;
                const bool ok = eng.resolve_market(id, s);
                std::cout << (ok ? "  ok\n" : "  rejected: invalid market id or already resolved\n");
            } else {
                std::cout << "  unknown 'market' subcommand\n";
            }
        } else if (cmd == "trader") {
            std::string maybe_create;
            iss >> maybe_create;
            if (maybe_create == "create") {
                Cash cash;
                iss >> cash;
                const TraderId id = eng.create_trader(cash);
                std::cout << "  created trader " << id << " with " << cash << " cents\n";
            } else {
                try {
                    dump_trader(eng, static_cast<TraderId>(std::stoul(maybe_create)));
                } catch (const std::exception&) {
                    std::cout << "  usage: trader <id>  |  trader create <cash_cents>\n";
                }
            }
        } else if (cmd == "buy" || cmd == "sell") {
            MarketId m; TraderId t; std::string contract_s; Price price; Qty qty; std::string tif_s;
            iss >> m >> t >> contract_s >> price >> qty >> tif_s;
            const TimeInForce tif = (tif_s == "ioc") ? TimeInForce::IOC : TimeInForce::GTC;
            const Side side = (cmd == "buy") ? Side::Buy : Side::Sell;
            const auto r = eng.submit(m, t, parse_contract(contract_s), side, OrderType::Limit, tif, price, qty);
            print_submit_result(r);
        } else if (cmd == "mbuy" || cmd == "msell") {
            MarketId m; TraderId t; std::string contract_s; Qty qty;
            iss >> m >> t >> contract_s >> qty;
            const Side side = (cmd == "mbuy") ? Side::Buy : Side::Sell;
            const auto r = eng.submit(m, t, parse_contract(contract_s), side, OrderType::Market, TimeInForce::IOC, 0, qty);
            print_submit_result(r);
        } else if (cmd == "cancel") {
            MarketId m; OrderId id;
            iss >> m >> id;
            const RejectReason r = eng.cancel(m, id);
            std::cout << "  -> " << reject_str(r) << "\n";
        } else if (cmd == "book") {
            MarketId m;
            iss >> m;
            dump_book(eng, m);
        } else if (cmd == "trades") {
            dump_trades(eng);
        } else {
            std::cout << "  unknown command '" << cmd << "' (try 'help')\n";
        }
    }
    return 0;
}
