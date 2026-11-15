// venue/wire.hpp — the order-entry protocol as bytes, with no socket in it.
//
// A small binary protocol in the style of an exchange's native order entry
// (Nasdaq's OUCH is the well-known one): fixed little-endian fields, one byte
// of message type, and a two-byte length in front of every message, because
// TCP delivers a stream of bytes and not messages. A read may return half a
// message or three and a half; FrameReader is the piece that puts them back
// together, and it is tested by feeding it one byte at a time.
//
//   client -> venue   'L' logon      session u32, owner u32
//                     'O' order      client_seq u64, then the request (30 bytes)
//                     'H' heartbeat
//   venue -> client   'A' logon ack  the next client_seq the venue expects, u64
//                     'R' report     a private report (venue/feed.hpp), 34 bytes
//                     'N' not taken  client_seq u64, why u8 (venue::Admit)
//                     'H' heartbeat
//
// The logon ack carries the sequence number the venue expects next, so a
// client that reconnects knows what to resend and what already arrived.
//
// This file does no I/O. tools/netdemo.cpp is the only place a socket appears.
#pragma once

#include "px/messages.hpp"
#include "venue/feed.hpp"
#include "venue/journal.hpp"

#include <cstdint>
#include <vector>

namespace px::venue {

inline constexpr std::size_t kMaxFrame = 256;

struct Frame {
    char type = 0;
    std::vector<std::uint8_t> body;
};

inline void append_frame(std::vector<std::uint8_t>& out, char type, const std::uint8_t* body, std::size_t n) {
    out.push_back(static_cast<std::uint8_t>((n + 1) & 0xff));
    out.push_back(static_cast<std::uint8_t>((n + 1) >> 8));
    out.push_back(static_cast<std::uint8_t>(type));
    out.insert(out.end(), body, body + n);
}

inline void frame_logon(std::vector<std::uint8_t>& out, std::uint32_t session, OwnerId owner) {
    std::uint8_t b[8], *p = b;
    detail::put(p, session, 4);
    detail::put(p, owner, 4);
    append_frame(out, 'L', b, sizeof(b));
}
inline void frame_order(std::vector<std::uint8_t>& out, std::uint64_t client_seq, const OrderRequest& req) {
    std::uint8_t b[38], *p = b;
    detail::put(p, client_seq, 8);
    detail::encode_request(p, req);
    append_frame(out, 'O', b, sizeof(b));
}
inline void frame_heartbeat(std::vector<std::uint8_t>& out) { append_frame(out, 'H', nullptr, 0); }
inline void frame_logon_ack(std::vector<std::uint8_t>& out, std::uint64_t expected) {
    std::uint8_t b[8], *p = b;
    detail::put(p, expected, 8);
    append_frame(out, 'A', b, sizeof(b));
}
inline void frame_not_taken(std::vector<std::uint8_t>& out, std::uint64_t client_seq, std::uint8_t why) {
    std::uint8_t b[9], *p = b;
    detail::put(p, client_seq, 8);
    detail::put(p, why, 1);
    append_frame(out, 'N', b, sizeof(b));
}
inline void frame_report(std::vector<std::uint8_t>& out, const Report& r) {
    std::uint8_t b[34], *p = b;
    detail::put(p, r.seq, 8);
    detail::put(p, r.order_id, 8);
    detail::put(p, static_cast<std::uint64_t>(r.price), 8);
    detail::put(p, r.qty, 4);
    detail::put(p, r.leaves, 4);
    detail::put(p, static_cast<std::uint64_t>(r.type), 1);
    detail::put(p, r.reason, 1);
    append_frame(out, 'R', b, sizeof(b));
}

// Decoders: false if the body is the wrong length for its type.
[[nodiscard]] inline bool parse_logon(const Frame& f, std::uint32_t& session, OwnerId& owner) {
    if (f.type != 'L' || f.body.size() != 8) return false;
    const std::uint8_t* p = f.body.data();
    session = static_cast<std::uint32_t>(detail::get(p, 4));
    owner = static_cast<OwnerId>(detail::get(p, 4));
    return true;
}
[[nodiscard]] inline bool parse_order(const Frame& f, std::uint64_t& client_seq, OrderRequest& req) {
    if (f.type != 'O' || f.body.size() != 38) return false;
    const std::uint8_t* p = f.body.data();
    client_seq = detail::get(p, 8);
    req = detail::decode_request(p);
    return true;
}
[[nodiscard]] inline bool parse_u64(const Frame& f, char type, std::uint64_t& v) {
    if (f.type != type || f.body.size() < 8) return false;
    const std::uint8_t* p = f.body.data();
    v = detail::get(p, 8);
    return true;
}
[[nodiscard]] inline bool parse_report(const Frame& f, Report& r) {
    if (f.type != 'R' || f.body.size() != 34) return false;
    const std::uint8_t* p = f.body.data();
    r.seq = detail::get(p, 8);
    r.order_id = detail::get(p, 8);
    r.price = static_cast<Price>(detail::get(p, 8));
    r.qty = static_cast<Qty>(detail::get(p, 4));
    r.leaves = static_cast<Qty>(detail::get(p, 4));
    r.type = static_cast<ReportType>(detail::get(p, 1));
    r.reason = static_cast<std::uint8_t>(detail::get(p, 1));
    return true;
}

// Turns a byte stream back into messages, however the bytes were split.
class FrameReader {
public:
    void feed(const std::uint8_t* data, std::size_t n) { buf_.insert(buf_.end(), data, data + n); }

    // True and fills `f` if a whole message is waiting. `broken()` turns true,
    // for good, on a length no message can have: the stream cannot be trusted
    // after that and the connection should be dropped.
    [[nodiscard]] bool next(Frame& f) {
        if (broken_ || buf_.size() - at_ < 2) return false;
        const std::size_t len = buf_[at_] | (static_cast<std::size_t>(buf_[at_ + 1]) << 8);
        if (len == 0 || len > kMaxFrame) {
            broken_ = true;
            return false;
        }
        if (buf_.size() - at_ < 2 + len) return false;
        f.type = static_cast<char>(buf_[at_ + 2]);
        f.body.assign(buf_.begin() + static_cast<std::ptrdiff_t>(at_ + 3),
                      buf_.begin() + static_cast<std::ptrdiff_t>(at_ + 2 + len));
        at_ += 2 + len;
        if (at_ == buf_.size()) {
            buf_.clear();
            at_ = 0;
        }
        return true;
    }
    [[nodiscard]] bool broken() const noexcept { return broken_; }

private:
    std::vector<std::uint8_t> buf_;
    std::size_t at_ = 0;
    bool broken_ = false;
};

} // namespace px::venue
