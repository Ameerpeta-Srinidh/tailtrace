// TailTrace - wire format.
//
// A packet looks like a real Nasdaq feed packet:
//
//   +-------------------------+  MoldUDP64 header (20 bytes, the real thing)
//   | session[10] seq u64 cnt |    seq = sequence number of the FIRST message
//   +-------------------------+    cnt = messages in this packet (0xFFFF = end)
//   | TailTrace timing (16 B) |  NOT part of MoldUDP64: when the packet was
//   |  sched_ns u64 send_ns   |    *supposed* to go out, and when it did
//   +-------------------------+
//   | len u16 | ITCH 5.0 msg  |  repeated cnt times ("message blocks")
//   +-------------------------+
//
// Everything is big-endian ("network byte order"), exactly as in ITCH. The
// message bodies follow the ITCH 5.0 spec byte for byte for the three types
// we use: 'A' Add Order, 'E' Order Executed, 'X' Order Cancel. Timestamps in
// ITCH are 6 bytes (nanoseconds since midnight), so we have helpers for u48.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace tt::wire {

// ---- big-endian load/store -------------------------------------------------
// Written byte by byte on purpose: no alignment assumptions, no UB, and the
// compiler turns each one into a single bswap+mov anyway.
inline void put_u16(std::uint8_t* p, std::uint16_t v) { p[0] = v >> 8; p[1] = v; }
inline void put_u32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
}
inline void put_u48(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 6; ++i) p[i] = static_cast<std::uint8_t>(v >> (40 - 8 * i));
}
inline void put_u64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (56 - 8 * i));
}
inline std::uint16_t get_u16(const std::uint8_t* p) { return std::uint16_t(p[0] << 8 | p[1]); }
inline std::uint32_t get_u32(const std::uint8_t* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v = v << 8 | p[i];
    return v;
}
inline std::uint64_t get_u48(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 6; ++i) v = v << 8 | p[i];
    return v;
}
inline std::uint64_t get_u64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = v << 8 | p[i];
    return v;
}

// ---- packet header ---------------------------------------------------------
inline constexpr std::size_t kSessionLen = 10;
inline constexpr std::size_t kMoldHeaderLen = 20;
inline constexpr std::size_t kTimingLen = 16;
inline constexpr std::size_t kHeaderLen = kMoldHeaderLen + kTimingLen;  // 36
inline constexpr std::uint16_t kEndOfSession = 0xFFFF;
inline constexpr std::size_t kMaxPacket = 1400;  // stay under a 1500-byte MTU
inline constexpr char kSession[kSessionLen + 1] = "TAILTRACE1";

struct PacketHeader {
    std::uint64_t seq = 0;       // sequence number of the first message
    std::uint16_t count = 0;     // number of messages, or kEndOfSession
    std::uint64_t sched_ns = 0;  // intended send time (CLOCK_REALTIME)
    std::uint64_t send_ns = 0;   // actual send time   (CLOCK_REALTIME)
};

inline void write_header(std::uint8_t* p, const PacketHeader& h) {
    std::memcpy(p, kSession, kSessionLen);
    put_u64(p + 10, h.seq);
    put_u16(p + 18, h.count);
    put_u64(p + 20, h.sched_ns);
    put_u64(p + 28, h.send_ns);
}

// The sender stamps send_ns as late as possible, after the payload is built.
inline void patch_send_ns(std::uint8_t* p, std::uint64_t send_ns) { put_u64(p + 28, send_ns); }

inline std::optional<PacketHeader> read_header(std::span<const std::uint8_t> pkt) {
    if (pkt.size() < kHeaderLen) return std::nullopt;
    if (std::memcmp(pkt.data(), kSession, kSessionLen) != 0) return std::nullopt;
    PacketHeader h;
    h.seq = get_u64(pkt.data() + 10);
    h.count = get_u16(pkt.data() + 18);
    h.sched_ns = get_u64(pkt.data() + 20);
    h.send_ns = get_u64(pkt.data() + 28);
    return h;
}

// ---- ITCH 5.0 messages -----------------------------------------------------
// Field offsets below are straight from the TotalView-ITCH 5.0 spec.
inline constexpr std::size_t kAddLen = 36;       // 'A'
inline constexpr std::size_t kExecutedLen = 31;  // 'E'
inline constexpr std::size_t kCancelLen = 23;    // 'X'

struct AddOrder {
    std::uint16_t stock_locate = 0;
    std::uint16_t tracking = 0;
    std::uint64_t timestamp = 0;  // ns since midnight, 48 bits on the wire
    std::uint64_t order_ref = 0;
    char side = 'B';              // 'B' buy, 'S' sell
    std::uint32_t shares = 0;
    char stock[8] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};  // right-padded
    std::uint32_t price = 0;      // fixed point, 4 decimals (1234500 = 123.45)
};
struct OrderExecuted {
    std::uint16_t stock_locate = 0;
    std::uint16_t tracking = 0;
    std::uint64_t timestamp = 0;
    std::uint64_t order_ref = 0;
    std::uint32_t shares = 0;
    std::uint64_t match_number = 0;
};
struct OrderCancel {
    std::uint16_t stock_locate = 0;
    std::uint16_t tracking = 0;
    std::uint64_t timestamp = 0;
    std::uint64_t order_ref = 0;
    std::uint32_t shares = 0;
};

inline void encode(std::uint8_t* p, const AddOrder& m) {
    p[0] = 'A';
    put_u16(p + 1, m.stock_locate);
    put_u16(p + 3, m.tracking);
    put_u48(p + 5, m.timestamp);
    put_u64(p + 11, m.order_ref);
    p[19] = static_cast<std::uint8_t>(m.side);
    put_u32(p + 20, m.shares);
    std::memcpy(p + 24, m.stock, 8);
    put_u32(p + 32, m.price);
}
inline void encode(std::uint8_t* p, const OrderExecuted& m) {
    p[0] = 'E';
    put_u16(p + 1, m.stock_locate);
    put_u16(p + 3, m.tracking);
    put_u48(p + 5, m.timestamp);
    put_u64(p + 11, m.order_ref);
    put_u32(p + 19, m.shares);
    put_u64(p + 23, m.match_number);
}
inline void encode(std::uint8_t* p, const OrderCancel& m) {
    p[0] = 'X';
    put_u16(p + 1, m.stock_locate);
    put_u16(p + 3, m.tracking);
    put_u48(p + 5, m.timestamp);
    put_u64(p + 11, m.order_ref);
    put_u32(p + 19, m.shares);
}

inline AddOrder decode_add(const std::uint8_t* p) {
    AddOrder m;
    m.stock_locate = get_u16(p + 1);
    m.tracking = get_u16(p + 3);
    m.timestamp = get_u48(p + 5);
    m.order_ref = get_u64(p + 11);
    m.side = static_cast<char>(p[19]);
    m.shares = get_u32(p + 20);
    std::memcpy(m.stock, p + 24, 8);
    m.price = get_u32(p + 32);
    return m;
}
inline OrderExecuted decode_executed(const std::uint8_t* p) {
    OrderExecuted m;
    m.stock_locate = get_u16(p + 1);
    m.tracking = get_u16(p + 3);
    m.timestamp = get_u48(p + 5);
    m.order_ref = get_u64(p + 11);
    m.shares = get_u32(p + 19);
    m.match_number = get_u64(p + 23);
    return m;
}
inline OrderCancel decode_cancel(const std::uint8_t* p) {
    OrderCancel m;
    m.stock_locate = get_u16(p + 1);
    m.tracking = get_u16(p + 3);
    m.timestamp = get_u48(p + 5);
    m.order_ref = get_u64(p + 11);
    m.shares = get_u32(p + 19);
    return m;
}

inline std::size_t body_len(char type) {
    switch (type) {
        case 'A': return kAddLen;
        case 'E': return kExecutedLen;
        case 'X': return kCancelLen;
        default: return 0;
    }
}

// ---- packet builder --------------------------------------------------------
// Appends length-prefixed message blocks after the header.
class PacketBuilder {
public:
    void reset(std::uint64_t first_seq, std::uint64_t sched_ns) {
        hdr_ = {first_seq, 0, sched_ns, 0};
        len_ = kHeaderLen;
    }
    template <class Msg>
    bool add(const Msg& m, std::size_t body) {
        if (len_ + 2 + body > kMaxPacket) return false;
        put_u16(buf_ + len_, static_cast<std::uint16_t>(body));
        encode(buf_ + len_ + 2, m);
        len_ += 2 + body;
        ++hdr_.count;
        return true;
    }
    bool add(const AddOrder& m) { return add(m, kAddLen); }
    bool add(const OrderExecuted& m) { return add(m, kExecutedLen); }
    bool add(const OrderCancel& m) { return add(m, kCancelLen); }

    // Writes the header (count is now final) and returns the finished bytes.
    std::span<std::uint8_t> finish() {
        write_header(buf_, hdr_);
        return {buf_, len_};
    }
    void make_end_of_session(std::uint64_t next_seq) {
        hdr_ = {next_seq, kEndOfSession, 0, 0};
        len_ = kHeaderLen;
    }
    std::uint16_t count() const { return hdr_.count; }

private:
    PacketHeader hdr_;
    std::size_t len_ = kHeaderLen;
    std::uint8_t buf_[kMaxPacket] = {};
};

// ---- packet parser ---------------------------------------------------------
// Walks the message blocks and validates every length. Returns the number of
// well-formed messages, or -1 if the packet is malformed. The receiver uses
// this as its "application work", so it touches every field like a real
// feed handler would (the checksum keeps the compiler from skipping it).
struct ParseResult {
    int messages = -1;
    std::uint64_t checksum = 0;
};

inline ParseResult parse_packet(std::span<const std::uint8_t> pkt, const PacketHeader& h) {
    ParseResult r{0, 0};
    std::size_t off = kHeaderLen;
    for (std::uint16_t i = 0; i < h.count; ++i) {
        if (off + 2 > pkt.size()) return {-1, 0};
        const std::size_t len = get_u16(pkt.data() + off);
        off += 2;
        if (len == 0 || off + len > pkt.size()) return {-1, 0};
        const std::uint8_t* p = pkt.data() + off;
        if (body_len(static_cast<char>(p[0])) != len) return {-1, 0};
        switch (p[0]) {
            case 'A': {
                const AddOrder m = decode_add(p);
                r.checksum += m.order_ref ^ m.price ^ m.shares;
                break;
            }
            case 'E': {
                const OrderExecuted m = decode_executed(p);
                r.checksum += m.order_ref ^ m.match_number ^ m.shares;
                break;
            }
            case 'X': {
                const OrderCancel m = decode_cancel(p);
                r.checksum += m.order_ref ^ m.shares;
                break;
            }
        }
        off += len;
        ++r.messages;
    }
    return r;
}

}  // namespace tt::wire
