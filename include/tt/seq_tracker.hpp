// TailTrace - sequence-number tracking: gaps, late fills, duplicates.
//
// UDP gives no delivery guarantees. A feed handler's first job is to notice
// when the network lost, reordered or duplicated something. Every MoldUDP64
// packet says "my first message is #seq and I carry cnt messages", so the
// receiver knows exactly which message numbers it should see next.
//
//   expected = 10, packet [10, 13)   -> in order, expected = 13
//   expected = 13, packet [16, 18)   -> GAP: 13,14,15 missing, expected = 18
//   expected = 18, packet [14, 15)   -> late fill: 14 was reordered, not lost
//   expected = 18, packet [16, 18)   -> duplicate
//
// Anything still missing at the end of the run counts as lost. In v0.1 we only
// detect; a real handler would now ask a retransmit server for the missing
// range (that recovery path comes with the fault-injection milestone).
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace tt {

class SeqTracker {
public:
    struct Stats {
        std::uint64_t in_order = 0;     // messages that arrived when expected
        std::uint64_t gap_events = 0;   // times we jumped forward
        std::uint64_t gap_msgs = 0;     // messages skipped by those jumps
        std::uint64_t late_fills = 0;   // skipped messages that showed up later
        std::uint64_t duplicates = 0;   // messages we had already seen
        std::uint64_t outstanding() const { return gap_msgs - late_fills; }
    };

    explicit SeqTracker(std::uint64_t first_expected = 1) : expected_(first_expected) {}

    // Feed one packet covering messages [seq, seq + count).
    void on_packet(std::uint64_t seq, std::uint64_t count) {
        for (std::uint64_t s = seq; s < seq + count; ++s) on_message(s);
    }

    void on_message(std::uint64_t s) {
        if (s == expected_) {
            ++stats_.in_order;
            ++expected_;
        } else if (s > expected_) {
            ++stats_.gap_events;
            stats_.gap_msgs += s - expected_;
            holes_.push_back({expected_, s});  // [expected, s) missing
            ++stats_.in_order;
            expected_ = s + 1;
        } else if (fill_hole(s)) {
            ++stats_.late_fills;
        } else {
            ++stats_.duplicates;
        }
    }

    // The end-of-session packet carries the next sequence number the sender
    // would have used, which exposes losses at the very end of the stream
    // (no later packet would ever reveal them otherwise).
    void on_end(std::uint64_t next_seq) {
        if (next_seq > expected_) {
            ++stats_.gap_events;
            stats_.gap_msgs += next_seq - expected_;
            holes_.push_back({expected_, next_seq});
            expected_ = next_seq;
        }
    }

    std::uint64_t expected() const { return expected_; }
    const Stats& stats() const { return stats_; }
    std::size_t open_holes() const { return holes_.size(); }

private:
    struct Hole {
        std::uint64_t lo, hi;  // [lo, hi)
    };

    // A late message punches a hole into up to two smaller holes.
    bool fill_hole(std::uint64_t s) {
        auto it = std::find_if(holes_.begin(), holes_.end(),
                               [s](const Hole& h) { return s >= h.lo && s < h.hi; });
        if (it == holes_.end()) return false;
        const Hole h = *it;
        holes_.erase(it);
        if (h.lo < s) holes_.push_back({h.lo, s});
        if (s + 1 < h.hi) holes_.push_back({s + 1, h.hi});
        return true;
    }

    std::uint64_t expected_;
    Stats stats_;
    std::vector<Hole> holes_;
};

}  // namespace tt
