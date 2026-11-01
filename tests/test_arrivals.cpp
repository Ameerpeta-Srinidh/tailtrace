// Arrivals: both schedules hit the requested mean rate; Hawkes is much
// burstier than Poisson at the same rate, and stays stable.
#include <cmath>
#include <vector>

#include "check.hpp"
#include "tt/arrivals.hpp"

static double dispersion(const std::vector<std::uint64_t>& t, std::uint64_t win, double dur) {
    const auto n = static_cast<std::size_t>(dur * 1e9 / double(win));
    std::vector<double> c(n, 0);
    for (auto x : t)
        if (x / win < n) c[x / win] += 1;
    double m = 0, v = 0;
    for (double x : c) m += x;
    m /= double(n);
    for (double x : c) v += (x - m) * (x - m);
    v /= double(n);
    return v / m;
}

int main() {
    const double rate = 50'000, dur = 20;
    const auto p = tt::poisson_schedule(rate, dur, 1);
    CHECK_NEAR(double(p.size()) / dur, rate, rate * 0.01);
    for (std::size_t i = 1; i < p.size(); ++i) CHECK(p[i] >= p[i - 1]);
    CHECK_NEAR(dispersion(p, 1'000'000, dur), 1.0, 0.1);  // Poisson: var == mean

    tt::HawkesParams hp;
    hp.mean_rate = rate;
    hp.branching = 0.7;
    hp.decay_s = 50e-6;
    const auto h = tt::hawkes_schedule(hp, dur, 1);
    CHECK_NEAR(double(h.size()) / dur, rate, rate * 0.03);
    bool sorted = true;
    for (std::size_t i = 1; i < h.size(); ++i) sorted = sorted && h[i] >= h[i - 1];
    CHECK(sorted);
    // Theory for windows much longer than the decay: dispersion -> 1/(1-n)^2 = 11.1
    const double d = dispersion(h, 1'000'000, dur);
    CHECK(d > 8.0 && d < 14.0);

    // Same seed, same schedule: runs are reproducible.
    CHECK(tt::hawkes_schedule(hp, 1, 7) == tt::hawkes_schedule(hp, 1, 7));
    return report("test_arrivals");
}
