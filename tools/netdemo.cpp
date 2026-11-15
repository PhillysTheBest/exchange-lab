// tools/netdemo.cpp — the venue behind a real TCP socket, on this machine only.
//
// Everything else in this repository runs in one process with no network, by
// decision, and that decision stands for the engine, the arena and every
// program that produces evidence. This one program is the exception, and it is
// a demonstration, not an experiment: it shows the part of an exchange that a
// function call cannot, which is a session.
//
// It starts a venue listening on 127.0.0.1 (a port the system picks), connects
// two clients to it from the same process, and plays a short script:
//
//   logon and the sequence number the venue expects
//   an order, a fill, and a private report to each side
//   a resend (dropped) and a skipped number (refused)
//   one message arriving in two pieces
//   cancel on disconnect: a client drops, its resting order is pulled
//   reconnect: the venue still knows where that session had got to
//
// It checks each step and exits 0 only if every one held, so `make test-net`
// is a test. It binds to the loopback address and nothing else; no packet
// leaves the machine. POSIX sockets only: on Windows it says so and exits 0.
#include "px/matching_engine.hpp"
#include "venue/venue.hpp"
#include "venue/wire.hpp"

#include <cstdio>

#ifdef _WIN32
int main() {
    std::printf("netdemo: built for POSIX sockets; not available on Windows\n");
    return 0;
}
#else

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <map>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace px;
using namespace px::venue;

namespace {

struct Conn {
    int fd = -1;
    FrameReader reader;
    bool logged_on = false;
    std::uint32_t session = 0;
    OwnerId owner = 0;
};

struct Server {
    int listen_fd = -1;
    std::uint16_t port = 0;
    std::atomic<bool> stop{false};
    std::atomic<int> disconnect_cancels{0};
    std::atomic<int> disconnects{0};

    bool open() {
        listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // this machine only
        a.sin_port = 0;
        if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(listen_fd, 8) != 0) return false;
        socklen_t len = sizeof(a);
        ::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        return true;
    }

    void run() {
        MatchingEngine engine;
        engine.add_instrument("DEMO", 100);
        std::vector<Conn> conns;
        std::map<OwnerId, std::vector<std::uint8_t>> outbox; // reports waiting for each owner
        Venue venue(engine, 4, RiskLimits{}, "", nullptr, [&outbox](const Report& r) { frame_report(outbox[r.owner], r); });

        const auto flush = [&] {
            for (Conn& c : conns) {
                if (!c.logged_on) continue;
                std::vector<std::uint8_t>& out = outbox[c.owner];
                if (!out.empty() && ::send(c.fd, out.data(), out.size(), 0) >= 0) out.clear();
            }
        };

        while (!stop.load()) {
            std::vector<pollfd> fds;
            fds.push_back({listen_fd, POLLIN, 0});
            for (const Conn& c : conns) fds.push_back({c.fd, POLLIN, 0});
            if (::poll(fds.data(), fds.size(), 20) <= 0) continue;
            if (fds[0].revents & POLLIN) {
                Conn c;
                c.fd = ::accept(listen_fd, nullptr, nullptr);
                if (c.fd >= 0) {
                    const int one = 1;
                    ::setsockopt(c.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    conns.push_back(std::move(c));
                }
            }
            for (std::size_t i = 1; i < fds.size(); ++i) {
                if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
                Conn& c = conns[i - 1];
                std::uint8_t buf[512];
                const ssize_t n = ::recv(c.fd, buf, sizeof(buf), 0);
                if (n <= 0) { // the client went away: nobody is watching its orders, so pull them
                    if (c.logged_on) disconnect_cancels += static_cast<int>(venue.pull_orders(c.owner));
                    ::close(c.fd);
                    c.fd = -1;
                    ++disconnects;
                    continue;
                }
                c.reader.feed(buf, static_cast<std::size_t>(n));
                Frame f;
                std::vector<std::uint8_t> reply;
                while (c.reader.next(f)) {
                    std::uint64_t cs = 0;
                    OrderRequest req;
                    if (parse_logon(f, c.session, c.owner)) {
                        c.logged_on = c.session < venue.sequencer().sessions();
                        if (c.logged_on) frame_logon_ack(reply, venue.sequencer().expected(c.session));
                    } else if (f.type == 'H') {
                        frame_heartbeat(reply);
                    } else if (c.logged_on && parse_order(f, cs, req)) {
                        req.owner = c.owner; // the session decides who you are, not the message
                        const SubmitResult r = venue.submit(c.session, cs, req);
                        if (r.admit != Admit::Accepted) frame_not_taken(reply, cs, static_cast<std::uint8_t>(r.admit));
                    }
                }
                if (!reply.empty()) ::send(c.fd, reply.data(), reply.size(), 0);
                if (c.reader.broken()) ::shutdown(c.fd, SHUT_RDWR);
            }
            for (auto it = conns.begin(); it != conns.end();) it = it->fd < 0 ? conns.erase(it) : it + 1;
            flush();
        }
        for (Conn& c : conns) ::close(c.fd);
        ::close(listen_fd);
    }
};

struct Client {
    int fd = -1;
    FrameReader reader;
    const char* name;

    bool connect_to(std::uint16_t port) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);
        const int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        return ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
    }
    void send_bytes(const std::uint8_t* p, std::size_t n) { (void)::send(fd, p, n, 0); }
    void send_all(const std::vector<std::uint8_t>& b) { send_bytes(b.data(), b.size()); }
    // Waits up to a second for the next whole message.
    bool read(Frame& f) {
        for (int tries = 0; tries < 50; ++tries) {
            if (reader.next(f)) return true;
            pollfd p{fd, POLLIN, 0};
            if (::poll(&p, 1, 20) <= 0) continue;
            std::uint8_t buf[512];
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) return false;
            reader.feed(buf, static_cast<std::size_t>(n));
        }
        return reader.next(f);
    }
    void hang_up() {
        ::close(fd);
        fd = -1;
    }
};

int g_failed = 0;
void step(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    g_failed += ok ? 0 : 1;
}

bool expect_report(Client& c, ReportType type, OrderId id, Qty qty, Qty leaves) {
    Frame f;
    Report r;
    return c.read(f) && parse_report(f, r) && r.type == type && r.order_id == id && r.qty == qty && r.leaves == leaves;
}

} // namespace

int main() {
    std::signal(SIGPIPE, SIG_IGN); // a write to a client that has gone is an error to handle, not a signal
    Server server;
    if (!server.open()) {
        std::printf("netdemo: could not listen on 127.0.0.1\n");
        return 1;
    }
    std::thread st([&server] { server.run(); });
    std::printf("netdemo: venue listening on 127.0.0.1:%u (loopback only)\n", server.port);
    const Price px = 100 * kPriceScale;
    Frame f;
    std::uint64_t v = 0;
    std::vector<std::uint8_t> b;

    Client alice{-1, {}, "alice"}, bob{-1, {}, "bob"};
    step(alice.connect_to(server.port) && bob.connect_to(server.port), "two clients connect");
    frame_logon(b, 0, 1);
    alice.send_all(b);
    step(alice.read(f) && parse_u64(f, 'A', v) && v == 1, "alice logs on; the venue expects her message 1");
    b.clear();
    frame_logon(b, 1, 2);
    bob.send_all(b);
    step(bob.read(f) && parse_u64(f, 'A', v) && v == 1, "bob logs on; the venue expects his message 1");

    b.clear();
    frame_order(b, 1, limit_order(1001, 0, 0, Side::Sell, px, 100));
    alice.send_all(b);
    step(expect_report(alice, ReportType::Accepted, 1001, 100, 100), "alice offers 100 at 100.00 and is told it was accepted");

    b.clear();
    frame_order(b, 1, limit_order(2001, 0, 0, Side::Buy, px, 40));
    bob.send_all(b);
    step(expect_report(bob, ReportType::Accepted, 2001, 40, 40) && expect_report(bob, ReportType::Filled, 2001, 40, 0),
         "bob buys 40 and gets his fill");
    step(expect_report(alice, ReportType::Filled, 1001, 40, 60), "alice gets her side of it: 40 filled, 60 left, no name attached");

    bob.send_all(b); // the same message again, as after a timeout
    step(bob.read(f) && parse_u64(f, 'N', v) && v == 1 && f.body[8] == static_cast<std::uint8_t>(Admit::Duplicate),
         "bob resends message 1: dropped as a duplicate, no second order");
    b.clear();
    frame_order(b, 5, limit_order(2005, 0, 0, Side::Buy, px, 1));
    bob.send_all(b);
    step(bob.read(f) && parse_u64(f, 'N', v) && v == 5 && f.body[8] == static_cast<std::uint8_t>(Admit::Gap),
         "bob skips to message 5: refused, the venue wants 2");

    b.clear();
    frame_order(b, 2, limit_order(2002, 0, 0, Side::Buy, px, 10));
    bob.send_bytes(b.data(), 17); // half a message...
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    bob.send_bytes(b.data() + 17, b.size() - 17); // ...and the rest
    step(expect_report(bob, ReportType::Accepted, 2002, 10, 10) && expect_report(bob, ReportType::Filled, 2002, 10, 0),
         "an order arriving in two pieces is put back together and fills");
    step(expect_report(alice, ReportType::Filled, 1001, 10, 50), "alice has 50 left resting");

    alice.hang_up();
    for (int i = 0; i < 100 && server.disconnects.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    step(server.disconnect_cancels.load() == 1, "alice drops; the venue pulls her resting order (cancel on disconnect)");
    b.clear();
    frame_order(b, 3, limit_order(2003, 0, 0, Side::Buy, px, 10));
    bob.send_all(b);
    step(expect_report(bob, ReportType::Accepted, 2003, 10, 10) && !bob.read(f), "bob bids again: accepted, and nothing left to trade with");

    Client again{-1, {}, "alice"};
    b.clear();
    frame_logon(b, 0, 1);
    step(again.connect_to(server.port), "alice reconnects");
    again.send_all(b);
    bool acked = false;
    while (again.read(f)) { // reports queued while she was away come first
        if (parse_u64(f, 'A', v)) {
            acked = true;
            break;
        }
    }
    step(acked && v == 2, "the venue still expects her message 2");

    again.hang_up();
    bob.hang_up();
    server.stop.store(true);
    st.join();
    std::printf("netdemo: %s\n", g_failed == 0 ? "every step held" : "a step FAILED");
    return g_failed == 0 ? 0 : 1;
}
#endif
