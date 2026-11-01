// TailTrace - log-linear latency histogram (the idea behind HdrHistogram).
//
// Why not just store every sample and sort? At 1M packets/s for a minute that
// is 60M numbers per stage. A histogram is fixed size, O(1) to record, and
// can be merged across threads and runs.
//
// Why not fixed-width buckets? Latency spans 100 ns to 100 ms. 1 us buckets
// waste memory at the top and blur everything at the bottom. Instead we keep
// a constant *relative* error:
//
//   * values below 256 ns get their own bucket (exact)
//   * every power-of-two range above that ([256,512), [512,1024), ...) is cut
//     into 128 equal sub-buckets, so a bucket is at most 1/128 = 0.8% wide
//     relative to the values in it
//
// That is 128 buckets per doubling: about 4,600 counters cover 1 ns to
// 18 minutes. Percentiles are reported as the bucket's upper edge, so a
// reported p99.9 is never lower than the true one (HdrHistogram does the same).
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace tt {

class Histogram {
public:
    static constexpr int kSubBits = 7;                    // 128 sub-buckets
    static constexpr std::uint64_t kHalf = 1ull << kSubBits;
    static constexpr std::uint64_t kLinear = kHalf * 2;   // 256 exact buckets
    static constexpr int kMaxBits = 40;                   // clamp at 2^40 ns
    static constexpr std::uint64_t kMaxValue = (1ull << kMaxBits) - 1;

    Histogram() : counts_(index_of(kMaxValue) + 1, 0) {}

    static std::size_t index_of(std::uint64_t v) {
        if (v < kLinear) return static_cast<std::size_t>(v);
        const int msb = 63 - std::countl_zero(v);  // v in [2^msb, 2^(msb+1))
        const int shift = msb - kSubBits;          // >= 1 here
        // (v >> shift) lands in [128, 256): the top 8 bits of v.
        return static_cast<std::size_t>(shift) * kHalf + (v >> shift);
    }
    // Smallest and largest value that map to bucket i.
    static std::uint64_t lowest_in(std::size_t i) {
        if (i < kLinear) return i;
        const std::uint64_t shift = i / kHalf - 1;
        return (i % kHalf + kHalf) << shift;
    }
    static std::uint64_t highest_in(std::size_t i) {
        if (i < kLinear) return i;
        const std::uint64_t shift = i / kHalf - 1;
        return lowest_in(i) + (1ull << shift) - 1;
    }

    // Latencies can come out negative if two clocks disagree (or a clock is
    // stepped). We never silently fold those into 0: they are counted apart.
    void record(std::int64_t v) {
        if (v < 0) {
            ++negative_;
            return;
        }
        auto u = static_cast<std::uint64_t>(v);
        if (u > kMaxValue) {
            ++clamped_;
            u = kMaxValue;
        }
        ++counts_[index_of(u)];
        ++total_;
        sum_ += static_cast<double>(u);
        min_ = std::min(min_, u);
        max_ = std::max(max_, u);
    }

    void merge(const Histogram& o) {
        for (std::size_t i = 0; i < counts_.size(); ++i) counts_[i] += o.counts_[i];
        total_ += o.total_;
        sum_ += o.sum_;
        negative_ += o.negative_;
        clamped_ += o.clamped_;
        min_ = std::min(min_, o.min_);
        max_ = std::max(max_, o.max_);
    }

    // p in [0, 100]. Walks the buckets until the running count reaches the
    // rank of the requested percentile.
    std::uint64_t percentile(double p) const {
        if (total_ == 0) return 0;
        const double rank = std::clamp(p, 0.0, 100.0) / 100.0 * static_cast<double>(total_);
        std::uint64_t target = static_cast<std::uint64_t>(rank);
        if (static_cast<double>(target) < rank || target == 0) ++target;  // ceil, at least 1
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < counts_.size(); ++i) {
            seen += counts_[i];
            if (seen >= target) return std::min(highest_in(i), max_);
        }
        return max_;
    }

    std::uint64_t count() const { return total_; }
    std::uint64_t negative() const { return negative_; }
    std::uint64_t clamped() const { return clamped_; }
    std::uint64_t min() const { return total_ ? min_ : 0; }
    std::uint64_t max() const { return max_; }
    double mean() const { return total_ ? sum_ / static_cast<double>(total_) : 0.0; }

    // One row per non-empty bucket: lowest, highest, count.
    bool write_csv(const std::string& path) const {
        std::FILE* f = std::fopen(path.c_str(), "w");
        if (!f) return false;
        std::fprintf(f, "low_ns,high_ns,count\n");
        for (std::size_t i = 0; i < counts_.size(); ++i)
            if (counts_[i])
                std::fprintf(f, "%llu,%llu,%llu\n", (unsigned long long)lowest_in(i),
                             (unsigned long long)highest_in(i), (unsigned long long)counts_[i]);
        std::fclose(f);
        return true;
    }

private:
    std::vector<std::uint64_t> counts_;
    std::uint64_t total_ = 0, negative_ = 0, clamped_ = 0;
    std::uint64_t min_ = ~0ull, max_ = 0;
    double sum_ = 0.0;
};

// The percentiles every TailTrace summary reports.
inline constexpr double kReportPercentiles[] = {50.0, 90.0, 99.0, 99.9, 99.99};

inline void write_summary_header(std::FILE* f) {
    std::fprintf(f, "stage,count,mean_ns,min_ns,p50_ns,p90_ns,p99_ns,p99.9_ns,p99.99_ns,max_ns,negative\n");
}
inline void write_summary_row(std::FILE* f, const char* stage, const Histogram& h) {
    std::fprintf(f, "%s,%llu,%.1f,%llu", stage, (unsigned long long)h.count(), h.mean(),
                 (unsigned long long)h.min());
    for (double p : kReportPercentiles) std::fprintf(f, ",%llu", (unsigned long long)h.percentile(p));
    std::fprintf(f, ",%llu,%llu\n", (unsigned long long)h.max(), (unsigned long long)h.negative());
}

}  // namespace tt
