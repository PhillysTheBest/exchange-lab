// venue/journal.hpp — the input journal: every request, numbered, on disk.
//
// A matching engine that gives the same output for the same input (the engine's
// determinism contract) can be rebuilt after a crash from nothing but its input.
// This file is that input, written down. Each record is one OrderRequest with the
// inbound sequence number the venue gave it, written field by field in
// little-endian order (never the struct's raw bytes, so padding cannot leak in),
// followed by a checksum of the record.
//
// A crash can leave a half-written record at the end of the file. The reader
// stops at the first record that is short or whose checksum does not match and
// reports how many bytes were good, so a torn tail is dropped, never replayed.
//
// What this does not do: it flushes through the C library and does not call
// fsync, so it survives the process dying, not the machine losing power. A real
// venue also replicates the journal to another machine before acknowledging.
//
// Not engine code: this allocates, does file I/O and is never on a timed path.
#pragma once

#include "px/messages.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace px::venue {

inline constexpr char kJournalMagic[8] = {'P', 'X', 'J', 'R', 'N', 'L', '0', '1'};
inline constexpr std::size_t kJournalRecordBytes = 46;

struct JournalRecord {
    std::uint64_t in_seq = 0;
    OrderRequest req;
};

namespace detail {

inline void put(std::uint8_t*& p, std::uint64_t v, int bytes) noexcept {
    for (int i = 0; i < bytes; ++i) *p++ = static_cast<std::uint8_t>(v >> (8 * i));
}
inline std::uint64_t get(const std::uint8_t*& p, int bytes) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v |= static_cast<std::uint64_t>(*p++) << (8 * i);
    return v;
}
inline std::uint64_t fnv(const std::uint8_t* p, std::size_t n) noexcept {
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

inline void encode_request(std::uint8_t*& p, const OrderRequest& r) noexcept {
    put(p, r.order_id, 8);
    put(p, static_cast<std::uint64_t>(r.price), 8);
    put(p, r.qty, 4);
    put(p, r.owner, 4);
    put(p, r.instr, 2);
    put(p, static_cast<std::uint64_t>(r.type), 1);
    put(p, static_cast<std::uint64_t>(r.ord), 1);
    put(p, static_cast<std::uint64_t>(r.tif), 1);
    put(p, static_cast<std::uint64_t>(r.side), 1);
}
inline OrderRequest decode_request(const std::uint8_t*& p) noexcept {
    OrderRequest r;
    r.order_id = get(p, 8);
    r.price = static_cast<Price>(get(p, 8));
    r.qty = static_cast<Qty>(get(p, 4));
    r.owner = static_cast<OwnerId>(get(p, 4));
    r.instr = static_cast<InstrumentId>(get(p, 2));
    r.type = static_cast<ReqType>(get(p, 1));
    r.ord = static_cast<OrdType>(get(p, 1));
    r.tif = static_cast<Tif>(get(p, 1));
    r.side = static_cast<Side>(get(p, 1));
    return r;
}

} // namespace detail

inline void encode_record(std::uint8_t out[kJournalRecordBytes], const JournalRecord& rec) noexcept {
    std::uint8_t* p = out;
    detail::put(p, rec.in_seq, 8);
    detail::encode_request(p, rec.req);
    detail::put(p, detail::fnv(out, kJournalRecordBytes - 8), 8);
}

// False if the checksum does not match.
[[nodiscard]] inline bool decode_record(const std::uint8_t in[kJournalRecordBytes], JournalRecord& rec) noexcept {
    const std::uint8_t* p = in;
    rec.in_seq = detail::get(p, 8);
    rec.req = detail::decode_request(p);
    return detail::get(p, 8) == detail::fnv(in, kJournalRecordBytes - 8);
}

class JournalWriter {
public:
    // Opens for append. A new file gets the header; an existing one is cut back
    // to `good_bytes` first when the caller has read it and found a torn tail.
    explicit JournalWriter(const std::string& path) : path_(path) {
        f_ = std::fopen(path.c_str(), "ab");
        if (f_ != nullptr && std::ftell(f_) == 0) std::fwrite(kJournalMagic, 1, sizeof(kJournalMagic), f_);
    }
    JournalWriter(const JournalWriter&) = delete;
    JournalWriter& operator=(const JournalWriter&) = delete;
    ~JournalWriter() {
        if (f_ != nullptr) std::fclose(f_);
    }
    [[nodiscard]] bool ok() const noexcept { return f_ != nullptr; }

    // Written and flushed before the caller hands the request to the engine.
    bool append(std::uint64_t in_seq, const OrderRequest& req) noexcept {
        std::uint8_t buf[kJournalRecordBytes];
        encode_record(buf, JournalRecord{in_seq, req});
        return f_ != nullptr && std::fwrite(buf, 1, sizeof(buf), f_) == sizeof(buf) && std::fflush(f_) == 0;
    }

private:
    std::string path_;
    std::FILE* f_ = nullptr;
};

// Reads forward and remembers where it stopped, so a standby can call
// read_more() again and again and receive only what was appended since.
class JournalReader {
public:
    explicit JournalReader(std::string path) : path_(std::move(path)) {}

    // Appends every complete, intact record after the last one returned.
    // Returns false if the file is missing or its header is wrong.
    bool read_more(std::vector<JournalRecord>& out) {
        std::FILE* f = std::fopen(path_.c_str(), "rb");
        if (f == nullptr) return false;
        bool good = true;
        if (offset_ == 0) {
            char magic[sizeof(kJournalMagic)];
            good = std::fread(magic, 1, sizeof(magic), f) == sizeof(magic) &&
                   std::equal(magic, magic + sizeof(magic), kJournalMagic);
            if (good) offset_ = sizeof(magic);
        }
        if (good && std::fseek(f, static_cast<long>(offset_), SEEK_SET) == 0) {
            std::uint8_t buf[kJournalRecordBytes];
            JournalRecord rec;
            while (std::fread(buf, 1, sizeof(buf), f) == sizeof(buf) && decode_record(buf, rec)) {
                out.push_back(rec);
                offset_ += sizeof(buf);
            }
        }
        std::fclose(f);
        return good;
    }
    // Bytes of the file that held intact records: anything after is a torn tail.
    [[nodiscard]] std::size_t good_bytes() const noexcept { return offset_; }

private:
    std::string path_;
    std::size_t offset_ = 0;
};

} // namespace px::venue
