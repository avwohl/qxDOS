/*
 * Emu88SlirpNet.h - libslirp NAT wrapper for the emu88 NE2000 bridge
 *
 * Thin C++ wrapper that gives the emu88 backend a working TCP/IP stack
 * by reusing the libslirp instance that is already statically linked
 * into the dosbox-core static library. The wrapper exposes a simple
 * send/receive/poll API that the emu88 dos_io subclass plugs into its
 * net_send / net_receive / net_available virtuals.
 *
 * NAT plan matches the DOSBox SlirpEthernetConnection in
 * dosbox-ios/ethernet_slirp_static.cpp:
 *   network  10.0.2.0/24
 *   host     10.0.2.2
 *   nameserver 10.0.2.3
 *   DHCP start 10.0.2.15
 *   MTU/MRU  1514 (14-byte ethernet header + 1500 payload)
 */

#ifndef QXDOS_EMU88_SLIRP_NET_H
#define QXDOS_EMU88_SLIRP_NET_H

#include <cstdint>
#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

extern "C" {
struct Slirp;
}

class Emu88SlirpNet {
public:
    Emu88SlirpNet();
    ~Emu88SlirpNet();

    Emu88SlirpNet(const Emu88SlirpNet &) = delete;
    Emu88SlirpNet &operator=(const Emu88SlirpNet &) = delete;

    // Lifecycle
    bool start();
    void stop();
    bool is_running() const { return slirp_ != nullptr; }

    // Guest TX → host (guest just sent an ethernet frame)
    void send_packet(const uint8_t *pkt, int len);

    // Host → guest (pull next pending RX frame; returns bytes copied)
    int receive_packet(uint8_t *buf, int max_len);
    bool packet_available();

    // Drive the host-side poll loop. Should be called periodically from
    // the emulator thread (e.g. once per run_batch). timeout_ms is the
    // poll timeout; pass 0 for non-blocking.
    void pump(int timeout_ms = 0);

    // Internal callback hooks (public so file-static C glue can reach them)
    int  add_poll_fd(int fd, int slirp_events);
    int  get_poll_revents(int idx);
    void register_fd(int fd);
    void unregister_fd(int fd);
    void enqueue_rx(const uint8_t *pkt, int len);

    struct Timer {
        void (*cb)(void *) = nullptr;
        void *cb_opaque = nullptr;
        int64_t expires_ns = 0;  // 0 = disarmed
    };
    Timer *timer_new(void (*cb)(void *), void *cb_opaque);
    void   timer_free(Timer *t);
    void   timer_mod(Timer *t, int64_t expire_ms);
    void   timers_run();

private:
    struct Slirp *slirp_ = nullptr;

    std::deque<std::vector<uint8_t>> rx_queue_;
    std::mutex                       rx_mutex_;

    // Poll-fd state mirrors SlirpEthernetConnection's design.
    std::vector<int>          registered_fds_;
    std::vector<struct pollfd> polls_;

    std::vector<Timer*> timers_;
};

#endif // QXDOS_EMU88_SLIRP_NET_H
