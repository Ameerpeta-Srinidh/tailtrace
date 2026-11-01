// TailTrace - packet arrival schedules: Poisson and Hawkes.
//
// Both return a vector of send times (ns offsets from the start of the run),
// computed BEFORE the run starts. Precomputing matters: the sender must not
// decide "when to send next" based on how the last send went, or a slow
// moment on the sender quietly removes load exactly when the system is
// struggling (one half of "coordinated omission").
//
// Poisson: events are independent, gaps are Exponential(rate). The classic
// benchmark assumption, and the reason most benchmarks look better than prod.
//
// Hawkes (self-exciting): every event raises the chance of more events soon
// after, then the excitement decays. Intensity:
//
//     lambda(t) = mu + sum over past events t_i of  alpha * exp(-beta * (t - t_i))
//
//   * mu        background rate (events/s)
//   * alpha     jump in intensity per event
//   * beta      decay speed (1/beta = how long the excitement lasts)
//   * n = alpha / beta is the "branching ratio": the expected number of
//     children each event triggers. Need n < 1 or the process explodes.
//   * long-run mean rate = mu / (1 - n)
//
// We parametrise by the things you actually want to control: mean rate,
// branching ratio n, and decay time. Same mean rate as Poisson, very
// different bursts: that is the whole experiment.
#pragma once

#include <cmath>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <vector>

namespace tt {

inline std::vector<std::uint64_t> poisson_schedule(double rate_per_s, double duration_s,
                                                   std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::exponential_distribution<double> gap(rate_per_s);
    std::vector<std::uint64_t> out;
    out.reserve(static_cast<std::size_t>(rate_per_s * duration_s * 1.1) + 16);
    double t = 0.0;
    for (;;) {
        t += gap(rng);
        if (t >= duration_s) break;
        out.push_back(static_cast<std::uint64_t>(t * 1e9));
    }
    return out;
}

struct HawkesParams {
    double mean_rate = 100'000;  // events/s, long-run average
    double branching = 0.7;      // n = alpha/beta in [0, 1)
    double decay_s = 50e-6;      // 1/beta
    double mu() const { return mean_rate * (1.0 - branching); }
    double beta() const { return 1.0 / decay_s; }
    double alpha() const { return branching * beta(); }
};

// Ogata's thinning algorithm, specialised to the exponential kernel.
//
// Between events the intensity only decays, so its current value is an upper
// bound for the near future. Propose the next event as if the rate stayed at
// that bound, then accept it with probability (true rate at that time) /
// (bound). Rejected proposals just move time forward. The excitation term
// S(t) = sum alpha*exp(-beta(t-t_i)) is tracked recursively, so each step is
// O(1) instead of summing over all past events.
inline std::vector<std::uint64_t> hawkes_schedule(const HawkesParams& p, double duration_s,
                                                  std::uint64_t seed) {
    if (p.branching < 0.0 || p.branching >= 1.0)
        throw std::invalid_argument("Hawkes branching ratio must be in [0, 1)");
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    const double mu = p.mu(), alpha = p.alpha(), beta = p.beta();

    std::vector<std::uint64_t> out;
    out.reserve(static_cast<std::size_t>(p.mean_rate * duration_s * 1.2) + 16);
    double t = 0.0, S = 0.0;
    for (;;) {
        const double bound = mu + S;
        const double w = -std::log(1.0 - U(rng)) / bound;  // Exponential(bound)
        t += w;
        if (t >= duration_s) break;
        S *= std::exp(-beta * w);                          // excitation decays
        if (U(rng) * bound <= mu + S) {                    // accept
            out.push_back(static_cast<std::uint64_t>(t * 1e9));
            S += alpha;                                     // ...and excites
        }
    }
    return out;
}

}  // namespace tt
