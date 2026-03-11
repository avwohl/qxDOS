/*
 * ethernet_slirp_stub.cpp — Stub SLIRP backend for iOS
 *
 * libslirp is not available on iOS. This provides the
 * SlirpEthernetConnection class so the linker is satisfied,
 * but Initialize() always returns false (no SLIRP networking).
 */

#include "dosbox.h"
#include "network/ethernet_slirp.h"

SlirpEthernetConnection::SlirpEthernetConnection() = default;
SlirpEthernetConnection::~SlirpEthernetConnection() = default;

bool SlirpEthernetConnection::Initialize([[maybe_unused]] Section* config)
{
    return false; // SLIRP not available on iOS
}

void SlirpEthernetConnection::SendPacket([[maybe_unused]] const uint8_t* packet,
                                          [[maybe_unused]] int len) {}

void SlirpEthernetConnection::GetPackets(
        [[maybe_unused]] std::function<int(const uint8_t*, int)> callback) {}

int SlirpEthernetConnection::ReceivePacket([[maybe_unused]] const uint8_t* packet,
                                            [[maybe_unused]] int len)
{
    return 0;
}

struct slirp_timer* SlirpEthernetConnection::TimerNew(
        [[maybe_unused]] SlirpTimerCb cb,
        [[maybe_unused]] void* cb_opaque)
{
    return nullptr;
}

void SlirpEthernetConnection::TimerFree([[maybe_unused]] struct slirp_timer* timer) {}
void SlirpEthernetConnection::TimerMod([[maybe_unused]] struct slirp_timer* timer,
                                        [[maybe_unused]] int64_t expire_time) {}

int SlirpEthernetConnection::PollAdd([[maybe_unused]] int fd,
                                      [[maybe_unused]] int slirp_events)
{
    return -1;
}

int SlirpEthernetConnection::PollGetSlirpRevents([[maybe_unused]] int idx) { return 0; }
void SlirpEthernetConnection::PollRegister([[maybe_unused]] int fd) {}
void SlirpEthernetConnection::PollUnregister([[maybe_unused]] int fd) {}
