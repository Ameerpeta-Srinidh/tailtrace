// TailTrace - clocks, CPU pinning and small command-line helpers.
#pragma once

#include <sched.h>
#include <time.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace tt {

// CLOCK_REALTIME is wall-clock time. We use it (and not CLOCK_MONOTONIC) for
// every timestamp that crosses the sender/receiver boundary because the
// kernel's software receive timestamps (SO_TIMESTAMPING) are in REALTIME.
// Both namespaces share one kernel, so they share this clock: one-way latency
// is meaningful here without PTP. Caveat: NTP can step REALTIME, so do not run
// experiments while the clock is being adjusted.
inline std::uint64_t now_realtime_ns() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return std::uint64_t(ts.tv_sec) * 1'000'000'000ull + std::uint64_t(ts.tv_nsec);
}

inline std::uint64_t to_ns(const timespec& ts) {
    return std::uint64_t(ts.tv_sec) * 1'000'000'000ull + std::uint64_t(ts.tv_nsec);
}

// Wait until an absolute REALTIME instant. Sleeping is cheap but the kernel
// wakes us up tens of microseconds late, so we sleep only while the target is
// far away and spin (burning the core) for the last stretch.
inline void wait_until_ns(std::uint64_t target_ns, std::uint64_t spin_window_ns = 100'000) {
    for (;;) {
        const std::uint64_t now = now_realtime_ns();
        if (now >= target_ns) return;
        const std::uint64_t left = target_ns - now;
        if (left > spin_window_ns) {
            const std::uint64_t nap = left - spin_window_ns;
            timespec ts{static_cast<time_t>(nap / 1'000'000'000ull),
                        static_cast<long>(nap % 1'000'000'000ull)};
            nanosleep(&ts, nullptr);
        }
    }
}

// Pin the calling thread to one CPU. Without this the scheduler may migrate
// the thread between cores mid-run, which shows up as random tail spikes
// (cold caches) that have nothing to do with the network.
inline bool pin_to_cpu(int cpu) {
    if (cpu < 0) return true;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        std::fprintf(stderr, "warning: could not pin to cpu %d: %s\n", cpu, std::strerror(errno));
        return false;
    }
    return true;
}

[[noreturn]] inline void die(const char* what) {
    std::fprintf(stderr, "fatal: %s: %s\n", what, std::strerror(errno));
    std::exit(1);
}

// Minimal "--key value" parser: arg("--rate", "100000").
class Args {
public:
    Args(int argc, char** argv) : argc_(argc), argv_(argv) {}
    std::string get(std::string_view key, std::string_view def) const {
        for (int i = 1; i + 1 < argc_; ++i)
            if (key == argv_[i]) return argv_[i + 1];
        return std::string(def);
    }
    double num(std::string_view key, double def) const {
        const std::string v = get(key, "");
        return v.empty() ? def : std::strtod(v.c_str(), nullptr);
    }
    bool flag(std::string_view key) const {
        for (int i = 1; i < argc_; ++i)
            if (key == argv_[i]) return true;
        return false;
    }

private:
    int argc_;
    char** argv_;
};

}  // namespace tt
