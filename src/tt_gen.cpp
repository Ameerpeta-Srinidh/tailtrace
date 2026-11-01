// TailTrace - traffic generator.
//
// Sends an ITCH-over-MoldUDP64 style multicast feed whose packet send times
// follow a precomputed Poisson or Hawkes schedule. It is "open loop": it never
// waits for the receiver, and if it falls behind it sends late but still
// stamps the packet with the time it SHOULD have gone out (sched_ns). The
// receiver can then measure latency from the intended time, which is the
// honest number (see "coordinated omission" in the docs).
//
// Usage (inside the tx namespace, see scripts/run_experiment.sh):
//   tt_gen --dest 239.77.0.1 --port 30001 --iface 10.77.0.1
//          --arrivals hawkes --rate 100000 --branching 0.7 --decay-us 50
//          --duration 10 --msgs-per-pkt 1 --seed 1 --cpu 1 --out results/run
//
// Fault injection (seeded, so a failing run can be replayed exactly):
//   --loss 0.001      drop this fraction of packets (sequence numbers still advance)
//   --dup 0.001       send this fraction twice
//   --reorder 0.001   hold this fraction back and send it after the next packet
// tc netem can do the same inside the kernel (scripts/netem.sh); doing it
// here as well works on kernels without the netem module and records exactly
// which packets were hit.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "tt/arrivals.hpp"
#include "tt/histogram.hpp"
#include "tt/sys.hpp"
#include "tt/wire.hpp"

namespace {

// Synthetic order flow: adds, cancels and executions on live orders, so the
// payload has realistic message types and sizes. The *timing* is what this
// project studies; the content just has to be plausible and parseable.
class OrderFlow {
public:
    explicit OrderFlow(std::uint64_t seed) : rng_(seed) {}

    template <class Builder>
    void next(Builder& b, std::uint64_t ts_ns) {
        const std::uint64_t ts = ts_ns % (86'400ull * 1'000'000'000ull);  // ns since midnight
        const double u = U_(rng_);
        if (live_.empty() || u < 0.55) {
            tt::wire::AddOrder m;
            m.stock_locate = 1;
            m.timestamp = ts;
            m.order_ref = next_ref_++;
            m.side = U_(rng_) < 0.5 ? 'B' : 'S';
            m.shares = 100 * (1 + static_cast<std::uint32_t>(U_(rng_) * 10));
            std::memcpy(m.stock, "TTRC    ", 8);
            m.price = 1'000'000 + static_cast<std::uint32_t>(U_(rng_) * 2000) * 100;
            b.add(m);
            live_.push_back(m.order_ref);
            return;
        }
        const std::size_t k = static_cast<std::size_t>(U_(rng_) * live_.size());
        const std::uint64_t ref = live_[k];
        live_[k] = live_.back();
        live_.pop_back();
        if (u < 0.90) {
            tt::wire::OrderCancel m;
            m.stock_locate = 1;
            m.timestamp = ts;
            m.order_ref = ref;
            m.shares = 100;
            b.add(m);
        } else {
            tt::wire::OrderExecuted m;
            m.stock_locate = 1;
            m.timestamp = ts;
            m.order_ref = ref;
            m.shares = 100;
            m.match_number = next_match_++;
            b.add(m);
        }
    }

private:
    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> U_{0.0, 1.0};
    std::vector<std::uint64_t> live_;
    std::uint64_t next_ref_ = 1, next_match_ = 1;
};

// Burstiness of a schedule: how many packets land in each fixed window.
// For Poisson the variance/mean ratio ("index of dispersion") is ~1; bursty
// traffic gives >> 1. We also report the busiest window.
struct Burstiness {
    double dispersion = 0;
    std::uint64_t max_in_window = 0;
    double mean_in_window = 0;
};
Burstiness burstiness(const std::vector<std::uint64_t>& offs, std::uint64_t window_ns,
                      double duration_s) {
    const auto n_windows = static_cast<std::size_t>(duration_s * 1e9 / double(window_ns));
    if (n_windows == 0) return {};
    std::vector<std::uint64_t> c(n_windows, 0);
    for (auto t : offs)
        if (t / window_ns < n_windows) ++c[t / window_ns];
    double sum = 0, sq = 0;
    std::uint64_t mx = 0;
    for (auto x : c) {
        sum += double(x);
        sq += double(x) * double(x);
        mx = std::max(mx, x);
    }
    const double mean = sum / double(n_windows);
    const double var = sq / double(n_windows) - mean * mean;
    return {mean > 0 ? var / mean : 0, mx, mean};
}

}  // namespace

int main(int argc, char** argv) {
    tt::Args a(argc, argv);
    const std::string dest = a.get("--dest", "239.77.0.1");
    const int port = static_cast<int>(a.num("--port", 30001));
    const std::string iface = a.get("--iface", "10.77.0.1");
    const std::string arrivals = a.get("--arrivals", "poisson");
    const double rate = a.num("--rate", 100'000);
    const double duration = a.num("--duration", 5);
    const int msgs_per_pkt = std::max(1, static_cast<int>(a.num("--msgs-per-pkt", 1)));
    const auto seed = static_cast<std::uint64_t>(a.num("--seed", 1));
    const int cpu = static_cast<int>(a.num("--cpu", -1));
    const std::string out = a.get("--out", ".");
    const auto start_delay_ns = static_cast<std::uint64_t>(a.num("--start-delay-ms", 200) * 1e6);
    const double p_loss = a.num("--loss", 0), p_dup = a.num("--dup", 0), p_reorder = a.num("--reorder", 0);

    // 1. Schedule first, before any packet is sent.
    std::vector<std::uint64_t> offs;
    tt::HawkesParams hp;
    if (arrivals == "hawkes") {
        hp.mean_rate = rate;
        hp.branching = a.num("--branching", 0.7);
        hp.decay_s = a.num("--decay-us", 50) * 1e-6;
        offs = tt::hawkes_schedule(hp, duration, seed);
    } else if (arrivals == "poisson") {
        offs = tt::poisson_schedule(rate, duration, seed);
    } else {
        std::fprintf(stderr, "unknown --arrivals %s (poisson|hawkes)\n", arrivals.c_str());
        return 2;
    }

    // 2. Socket. connect() on a UDP socket does not send anything; it fixes
    //    the destination so every send() skips the per-packet route lookup.
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) tt::die("socket");
    in_addr ifaddr{};
    inet_pton(AF_INET, iface.c_str(), &ifaddr);
    if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof(ifaddr)) != 0)
        tt::die("IP_MULTICAST_IF");
    const unsigned char ttl = 1, loop = 0;  // stay on this link; don't echo to ourselves
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    const int sndbuf = 4 << 20;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(static_cast<std::uint16_t>(port));
    inet_pton(AF_INET, dest.c_str(), &to.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr*>(&to), sizeof(to)) != 0) tt::die("connect");

    tt::pin_to_cpu(cpu);

    // 3. Send loop.
    tt::wire::PacketBuilder b;
    OrderFlow flow(seed ^ 0x9E3779B97F4A7C15ull);
    tt::Histogram lateness;
    std::uint64_t next_seq = 1, sent = 0, send_errors = 0;
    std::uint64_t inj_loss = 0, inj_dup = 0, inj_reorder = 0;
    std::mt19937_64 fault_rng(seed * 31 + 17);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    std::vector<std::uint8_t> held;  // packet waiting to be sent out of order
    auto xmit = [&](const std::uint8_t* p, std::size_t n) {
        if (send(fd, p, n, 0) < 0)
            ++send_errors;  // ENOBUFS etc: the packet is gone, and the receiver will see a gap
        else
            ++sent;
    };
    const std::uint64_t t0 = tt::now_realtime_ns() + start_delay_ns;

    for (const std::uint64_t off : offs) {
        const std::uint64_t sched = t0 + off;
        // Build the payload BEFORE waiting, so the wait->send gap is tiny.
        b.reset(next_seq, sched);
        for (int k = 0; k < msgs_per_pkt; ++k) flow.next(b, sched);
        auto bytes = b.finish();
        tt::wait_until_ns(sched);
        const std::uint64_t now = tt::now_realtime_ns();
        tt::wire::patch_send_ns(bytes.data(), now);
        const double u = U(fault_rng);
        if (u < p_loss) {
            ++inj_loss;
        } else if (u < p_loss + p_reorder && held.empty()) {
            held.assign(bytes.begin(), bytes.end());
            ++inj_reorder;
        } else {
            xmit(bytes.data(), bytes.size());
            if (u < p_loss + p_reorder + p_dup) {
                xmit(bytes.data(), bytes.size());
                ++inj_dup;
            }
            if (!held.empty()) {
                xmit(held.data(), held.size());
                held.clear();
            }
        }
        lateness.record(static_cast<std::int64_t>(now - sched));
        next_seq += b.count();
    }

    if (!held.empty()) xmit(held.data(), held.size());

    // 4. End of session, sent a few times because UDP may drop any one of them.
    for (int i = 0; i < 5; ++i) {
        b.make_end_of_session(next_seq);
        auto bytes = b.finish();
        send(fd, bytes.data(), bytes.size(), 0);
        usleep(2000);
    }
    close(fd);

    // 5. Report.
    const Burstiness b100 = burstiness(offs, 100'000, duration);
    const Burstiness b1m = burstiness(offs, 1'000'000, duration);
    if (std::FILE* f = std::fopen((out + "/gen_stats.csv").c_str(), "w")) {
        std::fprintf(f, "key,value\n");
        std::fprintf(f, "arrivals,%s\n", arrivals.c_str());
        std::fprintf(f, "target_rate_pps,%.0f\n", rate);
        std::fprintf(f, "achieved_rate_pps,%.0f\n", double(offs.size()) / duration);
        if (arrivals == "hawkes") {
            std::fprintf(f, "hawkes_mu,%.1f\nhawkes_alpha,%.1f\nhawkes_beta,%.1f\nhawkes_branching,%.3f\n",
                         hp.mu(), hp.alpha(), hp.beta(), hp.branching);
        }
        std::fprintf(f, "duration_s,%.3f\nseed,%llu\nmsgs_per_pkt,%d\n", duration,
                     (unsigned long long)seed, msgs_per_pkt);
        std::fprintf(f, "packets_scheduled,%zu\npackets_sent,%llu\nsend_errors,%llu\nmessages_sent,%llu\n",
                     offs.size(), (unsigned long long)sent, (unsigned long long)send_errors,
                     (unsigned long long)(next_seq - 1));
        std::fprintf(f, "injected_loss,%llu\ninjected_dup,%llu\ninjected_reorder,%llu\n",
                     (unsigned long long)inj_loss, (unsigned long long)inj_dup, (unsigned long long)inj_reorder);
        std::fprintf(f, "dispersion_100us,%.3f\nmax_pkts_in_100us,%llu\nmean_pkts_in_100us,%.3f\n",
                     b100.dispersion, (unsigned long long)b100.max_in_window, b100.mean_in_window);
        std::fprintf(f, "dispersion_1ms,%.3f\nmax_pkts_in_1ms,%llu\nmean_pkts_in_1ms,%.3f\n",
                     b1m.dispersion, (unsigned long long)b1m.max_in_window, b1m.mean_in_window);
        std::fclose(f);
    }
    if (std::FILE* f = std::fopen((out + "/gen_summary.csv").c_str(), "w")) {
        tt::write_summary_header(f);
        tt::write_summary_row(f, "sender_lateness", lateness);
        std::fclose(f);
    }
    lateness.write_csv(out + "/hist_sender_lateness_gen.csv");
    if (inj_loss + inj_dup + inj_reorder)
        std::printf("tt_gen: injected %llu losses, %llu duplicates, %llu reorders\n", (unsigned long long)inj_loss,
                    (unsigned long long)inj_dup, (unsigned long long)inj_reorder);
    std::printf("tt_gen: %s, %zu packets in %.1fs (%.0f pps), %llu send errors, "
                "lateness p50 %llu ns p99.9 %llu ns, dispersion(1ms) %.2f\n",
                arrivals.c_str(), offs.size(), duration, double(offs.size()) / duration,
                (unsigned long long)send_errors, (unsigned long long)lateness.percentile(50),
                (unsigned long long)lateness.percentile(99.9), b1m.dispersion);
    return 0;
}
