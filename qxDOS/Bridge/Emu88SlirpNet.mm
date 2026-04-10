/*
 * Emu88SlirpNet.mm - libslirp NAT wrapper for the emu88 NE2000 bridge
 *
 * The libslirp symbols are exported by the dosbox-core static library that
 * the qxDOS app already links (the same instance powers DOSBox networking).
 * We use them directly via the C API rather than going through the DOSBox
 * SlirpEthernetConnection wrapper, since that wrapper is tied to DOSBox's
 * Section/Config plumbing.
 */

#import <Foundation/Foundation.h>
#include "Emu88SlirpNet.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/time.h>
#include <mach/mach_time.h>
#include <cstring>
#include <cstdio>

extern "C" {
#include "libslirp.h"
}

namespace {

inline Emu88SlirpNet *as_self(void *opaque) {
    return static_cast<Emu88SlirpNet *>(opaque);
}

// Slirp → host: a frame arrived from the virtual network and should be
// queued for the guest to consume on its next net_receive() call.
ssize_t qx_slirp_send_packet(const void *buf, size_t len, void *opaque) {
    if (!len) return 0;
    as_self(opaque)->enqueue_rx(static_cast<const uint8_t *>(buf), (int)len);
    return (ssize_t)len;
}

void qx_slirp_guest_error(const char *msg, void *opaque) {
    (void)opaque;
    NSLog(@"[Emu88Slirp] guest error: %s", msg ? msg : "(null)");
}

int64_t qx_slirp_clock_get_ns(void *opaque) {
    (void)opaque;
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) mach_timebase_info(&tb);
    uint64_t now = mach_absolute_time();
    return (int64_t)((now * tb.numer) / tb.denom);
}

void *qx_slirp_timer_new(SlirpTimerCb cb, void *cb_opaque, void *opaque) {
    return as_self(opaque)->timer_new(cb, cb_opaque);
}

void qx_slirp_timer_free(void *timer, void *opaque) {
    as_self(opaque)->timer_free(static_cast<Emu88SlirpNet::Timer *>(timer));
}

void qx_slirp_timer_mod(void *timer, int64_t expire_time_ms, void *opaque) {
    as_self(opaque)->timer_mod(static_cast<Emu88SlirpNet::Timer *>(timer),
                               expire_time_ms);
}

int qx_slirp_add_poll(int fd, int events, void *opaque) {
    return as_self(opaque)->add_poll_fd(fd, events);
}

int qx_slirp_get_revents(int idx, void *opaque) {
    return as_self(opaque)->get_poll_revents(idx);
}

void qx_slirp_register_poll_fd(int fd, void *opaque) {
    as_self(opaque)->register_fd(fd);
}

void qx_slirp_unregister_poll_fd(int fd, void *opaque) {
    as_self(opaque)->unregister_fd(fd);
}

void qx_slirp_notify(void *opaque) {
    (void)opaque;
}

} // anonymous namespace

Emu88SlirpNet::Emu88SlirpNet() = default;

Emu88SlirpNet::~Emu88SlirpNet() {
    stop();
}

bool Emu88SlirpNet::start() {
    if (slirp_) return true;

    SlirpConfig cfg = {};
    cfg.version = 1;
    cfg.restricted = 0;
    cfg.disable_host_loopback = false;

    constexpr size_t kEthernetFrameSize = 14 + 1500;  // header + payload
    cfg.if_mtu = kEthernetFrameSize;
    cfg.if_mru = kEthernetFrameSize;

    cfg.enable_emu = false;
    cfg.in_enabled = true;

    inet_pton(AF_INET, "10.0.2.0",  &cfg.vnetwork);
    inet_pton(AF_INET, "255.255.255.0", &cfg.vnetmask);
    inet_pton(AF_INET, "10.0.2.2",  &cfg.vhost);
    inet_pton(AF_INET, "10.0.2.3",  &cfg.vnameserver);
    inet_pton(AF_INET, "10.0.2.15", &cfg.vdhcp_start);

    cfg.in6_enabled = false;
    cfg.vhostname = "qxDOS";

    SlirpCb cb = {};
    cb.send_packet        = qx_slirp_send_packet;
    cb.guest_error        = qx_slirp_guest_error;
    cb.clock_get_ns       = qx_slirp_clock_get_ns;
    cb.timer_new          = qx_slirp_timer_new;
    cb.timer_free         = qx_slirp_timer_free;
    cb.timer_mod          = qx_slirp_timer_mod;
    cb.register_poll_fd   = qx_slirp_register_poll_fd;
    cb.unregister_poll_fd = qx_slirp_unregister_poll_fd;
    cb.notify             = qx_slirp_notify;

    slirp_ = slirp_new(&cfg, &cb, this);
    if (!slirp_) {
        NSLog(@"[Emu88Slirp] slirp_new failed");
        return false;
    }
    NSLog(@"[Emu88Slirp] initialized (%s)", slirp_version_string());
    return true;
}

void Emu88SlirpNet::stop() {
    if (slirp_) {
        slirp_cleanup(slirp_);
        slirp_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(rx_mutex_);
        rx_queue_.clear();
    }
    registered_fds_.clear();
    polls_.clear();
    for (Timer *t : timers_) delete t;
    timers_.clear();
}

void Emu88SlirpNet::send_packet(const uint8_t *pkt, int len) {
    if (!slirp_ || len <= 0) return;
    slirp_input(slirp_, pkt, len);
}

bool Emu88SlirpNet::packet_available() {
    std::lock_guard<std::mutex> lock(rx_mutex_);
    return !rx_queue_.empty();
}

int Emu88SlirpNet::receive_packet(uint8_t *buf, int max_len) {
    std::lock_guard<std::mutex> lock(rx_mutex_);
    if (rx_queue_.empty()) return 0;
    auto &frame = rx_queue_.front();
    int n = (int)frame.size();
    if (n > max_len) n = max_len;
    std::memcpy(buf, frame.data(), (size_t)n);
    rx_queue_.pop_front();
    return n;
}

void Emu88SlirpNet::enqueue_rx(const uint8_t *pkt, int len) {
    if (len <= 0) return;
    std::lock_guard<std::mutex> lock(rx_mutex_);
    // Drop packets if the queue is unreasonably backed up — emu88 may be
    // paused (e.g. waiting for a key) and we don't want to grow without bound.
    if (rx_queue_.size() > 256) rx_queue_.pop_front();
    rx_queue_.emplace_back(pkt, pkt + len);
}

void Emu88SlirpNet::pump(int timeout_ms) {
    if (!slirp_) return;

    // Build the pollfd list: registered fds first (with read+write interest),
    // then any extra fds slirp_pollfds_fill wants to add via add_poll.
    polls_.clear();
    for (int fd : registered_fds_) {
        if (fd >= 0) add_poll_fd(fd, SLIRP_POLL_IN | SLIRP_POLL_OUT);
    }

    uint32_t slirp_timeout = (uint32_t)timeout_ms;
    slirp_pollfds_fill(slirp_, &slirp_timeout, qx_slirp_add_poll, this);

    int poll_ret = 0;
    if (!polls_.empty()) {
        // Cap timeout to whatever slirp told us, but never block longer than
        // the caller asked for. We pass 0 in the common case (non-blocking).
        int wait_ms = (int)slirp_timeout;
        if (wait_ms < 0) wait_ms = 0;
        if (wait_ms > timeout_ms) wait_ms = timeout_ms;
        poll_ret = ::poll(polls_.data(), polls_.size(), wait_ms);
    }
    slirp_pollfds_poll(slirp_, poll_ret < 0 ? 1 : 0, qx_slirp_get_revents, this);
    timers_run();
}

int Emu88SlirpNet::add_poll_fd(int fd, int slirp_events) {
    if (fd < 0) return fd;
    short events = 0;
    if (slirp_events & SLIRP_POLL_IN)  events |= POLLIN;
    if (slirp_events & SLIRP_POLL_OUT) events |= POLLOUT;
    if (slirp_events & SLIRP_POLL_PRI) events |= POLLPRI;
    struct pollfd p;
    p.fd = fd;
    p.events = events;
    p.revents = 0;
    polls_.push_back(p);
    return (int)(polls_.size() - 1);
}

int Emu88SlirpNet::get_poll_revents(int idx) {
    if (idx < 0 || idx >= (int)polls_.size()) return 0;
    short r = polls_[idx].revents;
    int out = 0;
    if (r & POLLIN)  out |= SLIRP_POLL_IN;
    if (r & POLLOUT) out |= SLIRP_POLL_OUT;
    if (r & POLLPRI) out |= SLIRP_POLL_PRI;
    if (r & POLLERR) out |= SLIRP_POLL_ERR;
    if (r & POLLHUP) out |= SLIRP_POLL_HUP;
    return out;
}

void Emu88SlirpNet::register_fd(int fd) {
    if (fd < 0) return;
    unregister_fd(fd);
    registered_fds_.push_back(fd);
}

void Emu88SlirpNet::unregister_fd(int fd) {
    for (auto it = registered_fds_.begin(); it != registered_fds_.end(); ) {
        if (*it == fd) it = registered_fds_.erase(it);
        else ++it;
    }
}

Emu88SlirpNet::Timer *Emu88SlirpNet::timer_new(void (*cb)(void *), void *cb_opaque) {
    auto *t = new Timer{cb, cb_opaque, 0};
    timers_.push_back(t);
    return t;
}

void Emu88SlirpNet::timer_free(Timer *t) {
    if (!t) return;
    for (auto it = timers_.begin(); it != timers_.end(); ++it) {
        if (*it == t) { timers_.erase(it); break; }
    }
    delete t;
}

void Emu88SlirpNet::timer_mod(Timer *t, int64_t expire_ms) {
    if (t) t->expires_ns = expire_ms * 1000000;
}

void Emu88SlirpNet::timers_run() {
    int64_t now = qx_slirp_clock_get_ns(nullptr);
    // Snapshot the timer list since callbacks may modify timers_.
    std::vector<Timer *> snapshot = timers_;
    for (Timer *t : snapshot) {
        if (t->expires_ns && t->expires_ns < now) {
            t->expires_ns = 0;
            if (t->cb) t->cb(t->cb_opaque);
        }
    }
}
