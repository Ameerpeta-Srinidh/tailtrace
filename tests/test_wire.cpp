// Wire format: byte layout matches ITCH 5.0, round trips are exact, and the
// parser rejects truncated or inconsistent packets.
#include "check.hpp"
#include "tt/wire.hpp"

using namespace tt::wire;

int main() {
    // Big-endian helpers: most significant byte first.
    std::uint8_t b[8];
    put_u32(b, 0x01020304);
    CHECK(b[0] == 1 && b[3] == 4);
    put_u48(b, 0x0000AABBCCDDEEFFull);
    CHECK(b[0] == 0xAA && b[5] == 0xFF);
    CHECK(get_u48(b) == 0xAABBCCDDEEFFull);

    // Add Order: fields at the offsets the ITCH spec gives.
    AddOrder a;
    a.stock_locate = 7;
    a.tracking = 9;
    a.timestamp = 34'200'000'000'123ull;  // 09:30:00.000000123
    a.order_ref = 0x1122334455667788ull;
    a.side = 'S';
    a.shares = 300;
    std::memcpy(a.stock, "AAPL    ", 8);
    a.price = 1'895'000;  // 189.5000
    std::uint8_t m[kAddLen];
    encode(m, a);
    CHECK(m[0] == 'A');
    CHECK(get_u16(m + 1) == 7);
    CHECK(get_u48(m + 5) == a.timestamp);
    CHECK(m[11] == 0x11 && m[18] == 0x88);
    CHECK(m[19] == 'S');
    CHECK(std::memcmp(m + 24, "AAPL    ", 8) == 0);
    CHECK(get_u32(m + 32) == 1'895'000);
    const AddOrder a2 = decode_add(m);
    CHECK(a2.order_ref == a.order_ref && a2.price == a.price && a2.shares == a.shares && a2.side == 'S');

    // Full packet round trip.
    PacketBuilder pb;
    pb.reset(42, 1'000);
    CHECK(pb.add(a));
    OrderExecuted e;
    e.order_ref = 5;
    e.shares = 100;
    e.match_number = 77;
    CHECK(pb.add(e));
    OrderCancel x;
    x.order_ref = 6;
    x.shares = 50;
    CHECK(pb.add(x));
    auto pkt = pb.finish();
    patch_send_ns(pkt.data(), 2'000);
    CHECK(pkt.size() == kHeaderLen + 3 * 2 + kAddLen + kExecutedLen + kCancelLen);
    const auto h = read_header(pkt);
    CHECK(h.has_value());
    CHECK(h->seq == 42 && h->count == 3 && h->sched_ns == 1'000 && h->send_ns == 2'000);
    const ParseResult r = parse_packet(pkt, *h);
    CHECK(r.messages == 3);

    // Truncation and lies are rejected, not read past the end.
    CHECK(parse_packet(pkt.first(pkt.size() - 1), *h).messages == -1);
    PacketHeader liar = *h;
    liar.count = 4;
    CHECK(parse_packet(pkt, liar).messages == -1);
    std::uint8_t bad[kMaxPacket];
    std::memcpy(bad, pkt.data(), pkt.size());
    put_u16(bad + kHeaderLen, kAddLen - 1);  // wrong length for an 'A'
    CHECK(parse_packet({bad, pkt.size()}, *h).messages == -1);
    CHECK(!read_header(pkt.first(10)).has_value());

    // Packets fill up at the MTU-safe size instead of overflowing.
    pb.reset(1, 0);
    int n = 0;
    while (pb.add(a)) ++n;
    CHECK(n == static_cast<int>((kMaxPacket - kHeaderLen) / (2 + kAddLen)));
    return report("test_wire");
}
