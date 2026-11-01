// TailTrace - receiver under test, with per-stage latency breakdown.
//
// For every packet we have up to six timestamps, all CLOCK_REALTIME:
//
//   sched     sender: when the packet was SUPPOSED to be sent
//   send      sender: just before send()
//   kernel    kernel: packet entered the receive path (SO_TIMESTAMPING)
//   app       us:     recvmmsg() returned this batch
//   p0 / p1   us:     before / after parsing this packet
//
// and the gaps between them are the stages:
//
//   sender_lateness  send   - sched   generator could not keep up
//   send_to_kernel   kernel - send    send syscall, veth, netem, softirq
//   kernel_to_app    app    - kernel  waiting in the socket queue + wakeup
//   batch_wait       p0     - app     waiting behind earlier packets of the batch
//   parse            p1     - p0      our own "application" work
//
//   end_to_end       p1 - send        what most benchmarks report
//   from_schedule    p1 - sched       the honest total (no coordinated omission)
//
// Usage (inside the rx namespace):
//   tt_recv --group 239.77.0.1 --port 30001 --iface 10.77.0.2 --mode block
//           --batch 32 --cpu 2 --out results/run --trace-every 10
#include <csignal>
#include <cstdio>
#include <string>
#include <vector>

#include "tt/histogram.hpp"
#include "tt/rx_kernel_udp.hpp"
#include "tt/seq_tracker.hpp"
#include "tt/sys.hpp"
#include "tt/wire.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

enum Stage { kLateness, kSendToKernel, kKernelToApp, kBatchWait, kParse, kEndToEnd, kFromSchedule, kStages };
constexpr const char* kStageNames[kStages] = {"sender_lateness", "send_to_kernel", "kernel_to_app",
                                              "batch_wait",      "parse",          "end_to_end",
                                              "from_schedule"};

struct TraceRow {
    std::uint64_t seq, sched, send, kernel, app, p0, p1;
    std::uint16_t count, batch_size, batch_idx;
};

std::int64_t diff(std::uint64_t later, std::uint64_t earlier) {
    return static_cast<std::int64_t>(later - earlier);
}

}  // namespace

int main(int argc, char** argv) {
    tt::Args a(argc, argv);
    tt::KernelUdpConfig cfg;
    cfg.group = a.get("--group", cfg.group);
    cfg.port = static_cast<int>(a.num("--port", cfg.port));
    cfg.iface = a.get("--iface", cfg.iface);
    cfg.batch = std::max(1, static_cast<int>(a.num("--batch", cfg.batch)));
    cfg.rcvbuf = static_cast<int>(a.num("--rcvbuf", cfg.rcvbuf));
    const std::string mode = a.get("--mode", "block");
    cfg.busy = mode == "busy";
    const int cpu = static_cast<int>(a.num("--cpu", -1));
    const std::string out = a.get("--out", ".");
    const double timeout_s = a.num("--timeout-s", 60);
    const auto trace_every = static_cast<std::uint64_t>(a.num("--trace-every", 0));
    const auto trace_max = static_cast<std::size_t>(a.num("--trace-max", 2'000'000));

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    tt::pin_to_cpu(cpu);

    tt::KernelUdpReceiver rx(cfg);
    // Tell the experiment script we have joined the group and it can start
    // the generator.
    if (std::FILE* f = std::fopen((out + "/ready").c_str(), "w")) std::fclose(f);

    tt::Histogram h[kStages];
    tt::Histogram batch_sizes;
    tt::SeqTracker seq;
    std::vector<TraceRow> trace;
    trace.reserve(std::min<std::size_t>(trace_max, 1 << 20));
    std::uint64_t packets = 0, messages = 0, malformed = 0, no_kernel_ts = 0, batches = 0, ends = 0;
    std::uint64_t checksum = 0, first_app = 0, last_app = 0;
    std::uint32_t kernel_drops = 0;
    bool done = false;

    const std::uint64_t deadline = tt::now_realtime_ns() + static_cast<std::uint64_t>(timeout_s * 1e9);
    while (!done && !g_stop) {
        tt::RxBatch* b = nullptr;
        const int n = rx.poll(b);
        if (n == 0) {
            if (tt::now_realtime_ns() > deadline) break;
            continue;
        }
        ++batches;
        batch_sizes.record(n);
        kernel_drops = std::max(kernel_drops, b->kernel_drops);
        if (!first_app) first_app = b->app_ns;
        last_app = b->app_ns;

        for (int i = 0; i < n; ++i) {
            const tt::RxPacket& p = b->pkts[static_cast<std::size_t>(i)];
            const std::span<const std::uint8_t> bytes{p.data, p.len};
            const std::uint64_t p0 = tt::now_realtime_ns();
            const auto hdr = tt::wire::read_header(bytes);
            if (!hdr) {
                ++malformed;
                continue;
            }
            if (hdr->count == tt::wire::kEndOfSession) {
                if (!ends++) seq.on_end(hdr->seq);
                done = true;  // finish this batch, then stop
                continue;
            }
            const tt::wire::ParseResult r = tt::wire::parse_packet(bytes, *hdr);
            const std::uint64_t p1 = tt::now_realtime_ns();
            if (r.messages < 0) {
                ++malformed;
                continue;
            }
            checksum += r.checksum;
            ++packets;
            messages += static_cast<std::uint64_t>(r.messages);
            seq.on_packet(hdr->seq, hdr->count);

            h[kLateness].record(diff(hdr->send_ns, hdr->sched_ns));
            h[kBatchWait].record(diff(p0, b->app_ns));
            h[kParse].record(diff(p1, p0));
            h[kEndToEnd].record(diff(p1, hdr->send_ns));
            h[kFromSchedule].record(diff(p1, hdr->sched_ns));
            if (p.kernel_ns) {
                h[kSendToKernel].record(diff(p.kernel_ns, hdr->send_ns));
                h[kKernelToApp].record(diff(b->app_ns, p.kernel_ns));
            } else {
                ++no_kernel_ts;
            }
            if (trace_every && packets % trace_every == 0 && trace.size() < trace_max)
                trace.push_back({hdr->seq, hdr->sched_ns, hdr->send_ns, p.kernel_ns, b->app_ns, p0, p1,
                                 hdr->count, static_cast<std::uint16_t>(n), static_cast<std::uint16_t>(i)});
        }
    }

    // ---- outputs --------------------------------------------------------------
    if (std::FILE* f = std::fopen((out + "/summary.csv").c_str(), "w")) {
        tt::write_summary_header(f);
        for (int s = 0; s < kStages; ++s) tt::write_summary_row(f, kStageNames[s], h[s]);
        tt::write_summary_row(f, "batch_size", batch_sizes);
        std::fclose(f);
    }
    for (int s = 0; s < kStages; ++s) h[s].write_csv(out + "/hist_" + kStageNames[s] + ".csv");

    const auto& st = seq.stats();
    if (std::FILE* f = std::fopen((out + "/recv_stats.csv").c_str(), "w")) {
        std::fprintf(f, "key,value\n");
        std::fprintf(f, "mode,%s\nbatch,%d\nrcvbuf_effective,%d\n", mode.c_str(), cfg.batch,
                     rx.effective_rcvbuf());
        std::fprintf(f, "packets,%llu\nmessages,%llu\nmalformed,%llu\nbatches,%llu\nmean_batch,%.3f\n",
                     (unsigned long long)packets, (unsigned long long)messages,
                     (unsigned long long)malformed, (unsigned long long)batches,
                     batches ? double(packets) / double(batches) : 0.0);
        std::fprintf(f, "no_kernel_timestamp,%llu\nkernel_socket_drops,%u\nend_of_session_seen,%llu\n",
                     (unsigned long long)no_kernel_ts, kernel_drops, (unsigned long long)ends);
        std::fprintf(f, "seq_in_order,%llu\nseq_gap_events,%llu\nseq_gap_msgs,%llu\nseq_late_fills,%llu\n"
                        "seq_duplicates,%llu\nseq_lost,%llu\n",
                     (unsigned long long)st.in_order, (unsigned long long)st.gap_events,
                     (unsigned long long)st.gap_msgs, (unsigned long long)st.late_fills,
                     (unsigned long long)st.duplicates, (unsigned long long)st.outstanding());
        std::fprintf(f, "receive_span_s,%.3f\nchecksum,%llu\n", double(last_app - first_app) / 1e9,
                     (unsigned long long)checksum);
        std::fclose(f);
    }
    if (!trace.empty()) {
        if (std::FILE* f = std::fopen((out + "/trace.csv").c_str(), "w")) {
            std::fprintf(f, "seq,count,sched_ns,send_ns,kernel_ns,app_ns,p0_ns,p1_ns,batch_size,batch_idx\n");
            for (const auto& r : trace)
                std::fprintf(f, "%llu,%u,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u\n", (unsigned long long)r.seq,
                             r.count, (unsigned long long)r.sched, (unsigned long long)r.send,
                             (unsigned long long)r.kernel, (unsigned long long)r.app,
                             (unsigned long long)r.p0, (unsigned long long)r.p1, r.batch_size, r.batch_idx);
            std::fclose(f);
        }
    }

    std::printf("tt_recv[%s]: %llu packets, %llu msgs, lost %llu, late %llu, dup %llu, kernel drops %u | "
                "from_schedule p50 %llu ns, p99 %llu ns, p99.9 %llu ns, max %llu ns\n",
                mode.c_str(), (unsigned long long)packets, (unsigned long long)messages,
                (unsigned long long)st.outstanding(), (unsigned long long)st.late_fills,
                (unsigned long long)st.duplicates, kernel_drops,
                (unsigned long long)h[kFromSchedule].percentile(50),
                (unsigned long long)h[kFromSchedule].percentile(99),
                (unsigned long long)h[kFromSchedule].percentile(99.9),
                (unsigned long long)h[kFromSchedule].max());
    return 0;
}
