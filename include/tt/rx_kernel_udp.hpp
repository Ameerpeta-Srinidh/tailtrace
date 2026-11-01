// TailTrace - receiver I/O layer A: ordinary kernel UDP sockets.
//
// Every receiver strategy (this one now; io_uring and AF_XDP later) fills the
// same RxBatch, so the parsing and measurement code never changes. Any
// difference in the results is then caused by the I/O path alone.
//
// What this one does beyond a textbook recvfrom() loop:
//   * IP_ADD_MEMBERSHIP     join the multicast group on a chosen interface
//   * SO_RCVBUF(FORCE)      bigger socket queue, so bursts queue instead of drop
//   * SO_TIMESTAMPING       kernel writes the time each packet entered the
//                           receive path; we read it from a control message
//   * SO_RXQ_OVFL           kernel tells us how many packets it dropped
//                           because our socket queue was full
//   * recvmmsg()            up to N packets per system call instead of one
//   * busy mode             MSG_DONTWAIT in a loop: never sleep, never wait
//                           for the scheduler to wake us
#pragma once

#include <arpa/inet.h>
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <string>
#include <vector>

#include "tt/sys.hpp"
#include "tt/wire.hpp"

namespace tt {

struct RxPacket {
    const std::uint8_t* data = nullptr;
    std::size_t len = 0;
    std::uint64_t kernel_ns = 0;  // 0 if the kernel gave no timestamp
};

struct RxBatch {
    std::vector<RxPacket> pkts;
    std::uint64_t app_ns = 0;          // when the batch reached user space
    std::uint32_t kernel_drops = 0;    // cumulative socket-queue drops (SO_RXQ_OVFL)
};

struct KernelUdpConfig {
    std::string group = "239.77.0.1";
    int port = 30001;
    std::string iface = "10.77.0.2";
    int batch = 32;
    int rcvbuf = 8 << 20;
    bool busy = false;
};

class KernelUdpReceiver {
public:
    explicit KernelUdpReceiver(const KernelUdpConfig& c) : cfg_(c) {
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) die("socket");
        const int one = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        // SO_RCVBUF is capped by net.core.rmem_max; SO_RCVBUFFORCE (root only)
        // ignores the cap. The kernel doubles whatever we ask for, to account
        // for its own bookkeeping, so getsockopt reports 2x.
        if (setsockopt(fd_, SOL_SOCKET, SO_RCVBUFFORCE, &cfg_.rcvbuf, sizeof(int)) != 0)
            setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &cfg_.rcvbuf, sizeof(int));

        // Bind to the GROUP address (not INADDR_ANY) so this socket only gets
        // this group's traffic even if other groups use the same port.
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<std::uint16_t>(cfg_.port));
        inet_pton(AF_INET, cfg_.group.c_str(), &addr.sin_addr);
        if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) die("bind");

        // Joining sends an IGMP report and tells the NIC/stack to accept the
        // group's MAC address on that interface.
        ip_mreq mreq{};
        inet_pton(AF_INET, cfg_.group.c_str(), &mreq.imr_multiaddr);
        inet_pton(AF_INET, cfg_.iface.c_str(), &mreq.imr_interface);
        if (setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0)
            die("IP_ADD_MEMBERSHIP");

        // RX_SOFTWARE: generate a timestamp in software when the packet enters
        // the stack. SOFTWARE: report software timestamps to us.
        const int ts_flags = SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
        if (setsockopt(fd_, SOL_SOCKET, SO_TIMESTAMPING, &ts_flags, sizeof(ts_flags)) != 0)
            die("SO_TIMESTAMPING");
        setsockopt(fd_, SOL_SOCKET, SO_RXQ_OVFL, &one, sizeof(one));

        // In blocking mode, wake up every 200 ms even with no traffic so the
        // caller can notice a timeout or Ctrl-C.
        timeval tv{0, 200'000};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        const auto n = static_cast<std::size_t>(cfg_.batch);
        bufs_.assign(n * wire::kMaxPacket, 0);
        ctrl_.assign(n * kCtrlLen, 0);
        iov_.resize(n);
        msgs_.resize(n);
        batch_.pkts.resize(n);
    }
    ~KernelUdpReceiver() { close(fd_); }
    KernelUdpReceiver(const KernelUdpReceiver&) = delete;
    KernelUdpReceiver& operator=(const KernelUdpReceiver&) = delete;

    int effective_rcvbuf() const {
        int v = 0;
        socklen_t l = sizeof(v);
        getsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &v, &l);
        return v;
    }

    // Returns the number of packets in the batch: 0 on timeout / nothing yet.
    int poll(RxBatch*& out) {
        const auto n = static_cast<std::size_t>(cfg_.batch);
        // recvmmsg overwrites lengths, so the headers are re-armed every call.
        for (std::size_t i = 0; i < n; ++i) {
            iov_[i] = {bufs_.data() + i * wire::kMaxPacket, wire::kMaxPacket};
            msgs_[i].msg_hdr = {};
            msgs_[i].msg_hdr.msg_iov = &iov_[i];
            msgs_[i].msg_hdr.msg_iovlen = 1;
            msgs_[i].msg_hdr.msg_control = ctrl_.data() + i * kCtrlLen;
            msgs_[i].msg_hdr.msg_controllen = kCtrlLen;
            msgs_[i].msg_len = 0;
        }
        // Blocking mode: MSG_WAITFORONE = block until at least one packet,
        // then grab whatever else is already queued (up to n) and return.
        const int flags = cfg_.busy ? MSG_DONTWAIT : MSG_WAITFORONE;
        const int got = recvmmsg(fd_, msgs_.data(), static_cast<unsigned>(n), flags, nullptr);
        if (got <= 0) {
            if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) die("recvmmsg");
            return 0;
        }
        batch_.app_ns = now_realtime_ns();
        batch_.pkts.resize(static_cast<std::size_t>(got));
        for (int i = 0; i < got; ++i) {
            RxPacket& p = batch_.pkts[static_cast<std::size_t>(i)];
            p.data = bufs_.data() + static_cast<std::size_t>(i) * wire::kMaxPacket;
            p.len = msgs_[i].msg_len;
            p.kernel_ns = 0;
            read_cmsgs(msgs_[i].msg_hdr, p);
        }
        out = &batch_;
        return got;
    }

private:
    static constexpr std::size_t kCtrlLen = 256;

    // Ancillary data ("control messages") ride alongside the payload. Each one
    // has a level/type; we pick out the two we asked for.
    void read_cmsgs(msghdr& h, RxPacket& p) {
        for (cmsghdr* c = CMSG_FIRSTHDR(&h); c; c = CMSG_NXTHDR(&h, c)) {
            if (c->cmsg_level != SOL_SOCKET) continue;
            if (c->cmsg_type == SO_TIMESTAMPING) {
                // ts[0] = software, ts[1] = deprecated, ts[2] = hardware (raw)
                const auto* ts = reinterpret_cast<const scm_timestamping*>(CMSG_DATA(c));
                p.kernel_ns = to_ns(ts->ts[0]);
            } else if (c->cmsg_type == SO_RXQ_OVFL) {
                std::uint32_t drops;
                std::memcpy(&drops, CMSG_DATA(c), sizeof(drops));
                batch_.kernel_drops = drops;
            }
        }
    }

    KernelUdpConfig cfg_;
    int fd_ = -1;
    std::vector<std::uint8_t> bufs_, ctrl_;
    std::vector<iovec> iov_;
    std::vector<mmsghdr> msgs_;
    RxBatch batch_;
};

}  // namespace tt
