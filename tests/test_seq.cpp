// Sequence tracker: in-order, gaps, late fills, duplicates and tail loss.
#include "check.hpp"
#include "tt/seq_tracker.hpp"

int main() {
    tt::SeqTracker s(1);
    s.on_packet(1, 3);  // 1,2,3
    CHECK(s.stats().in_order == 3 && s.expected() == 4);
    s.on_packet(7, 2);  // 4,5,6 missing
    CHECK(s.stats().gap_events == 1 && s.stats().gap_msgs == 3 && s.expected() == 9);
    s.on_packet(5, 1);  // late fill in the middle: hole splits in two
    CHECK(s.stats().late_fills == 1 && s.open_holes() == 2);
    s.on_packet(5, 1);  // same again: now a duplicate
    CHECK(s.stats().duplicates == 1);
    s.on_packet(4, 1);
    s.on_packet(6, 1);
    CHECK(s.stats().late_fills == 3 && s.open_holes() == 0 && s.stats().outstanding() == 0);
    s.on_packet(1, 2);  // old packet replayed
    CHECK(s.stats().duplicates == 3);
    s.on_end(12);       // sender says 9,10,11 existed; we never saw them
    CHECK(s.stats().outstanding() == 3);
    return report("test_seq");
}
