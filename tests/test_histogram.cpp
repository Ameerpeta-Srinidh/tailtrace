// Histogram: bucket math is continuous, and percentiles agree with an exact
// sort to within the promised 1/128 relative error.
#include <algorithm>
#include <random>
#include <vector>

#include "check.hpp"
#include "tt/histogram.hpp"

int main() {
    using tt::Histogram;
    // Every value maps to a bucket that contains it, and buckets tile the line.
    for (std::uint64_t v : std::initializer_list<std::uint64_t>{0ull, 1ull, 255ull, 256ull, 257ull, 511ull, 512ull, 1000ull, 123456789ull,
                            Histogram::kMaxValue}) {
        const auto i = Histogram::index_of(v);
        CHECK(Histogram::lowest_in(i) <= v && v <= Histogram::highest_in(i));
    }
    for (std::size_t i = 1; i < Histogram::index_of(Histogram::kMaxValue); ++i)
        CHECK(Histogram::lowest_in(i) == Histogram::highest_in(i - 1) + 1);

    // Log-normal latencies (a typical shape) vs exact percentiles.
    std::mt19937_64 rng(3);
    std::lognormal_distribution<double> ln(std::log(5000.0), 0.8);
    std::vector<std::uint64_t> xs;
    Histogram h;
    for (int i = 0; i < 200'000; ++i) {
        const auto v = static_cast<std::uint64_t>(ln(rng));
        xs.push_back(v);
        h.record(static_cast<std::int64_t>(v));
    }
    std::sort(xs.begin(), xs.end());
    for (double p : {50.0, 90.0, 99.0, 99.9, 99.99}) {
        const auto rank = static_cast<std::size_t>(std::ceil(p / 100.0 * xs.size())) - 1;
        const double exact = double(xs[rank]);
        const double got = double(h.percentile(p));
        CHECK(got >= exact);                      // upper edge: never optimistic
        CHECK((got - exact) / exact <= 1.0 / 128 + 1e-9);
    }
    CHECK(h.percentile(100) == xs.back());
    CHECK(h.count() == xs.size());

    // Negative values are counted apart, never mixed into the distribution.
    Histogram n;
    n.record(-5);
    n.record(10);
    CHECK(n.count() == 1 && n.negative() == 1 && n.percentile(50) == 10);

    // Merge equals recording everything into one.
    Histogram a, b, both;
    for (int i = 1; i <= 1000; ++i) {
        (i % 2 ? a : b).record(i * 37);
        both.record(i * 37);
    }
    a.merge(b);
    CHECK(a.count() == both.count() && a.percentile(99) == both.percentile(99));
    return report("test_histogram");
}
