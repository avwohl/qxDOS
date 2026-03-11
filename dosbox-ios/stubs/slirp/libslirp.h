/*
 * Stub libslirp.h — libslirp is not available on iOS.
 * Provides type definitions so ethernet_slirp.h compiles,
 * but actual SLIRP networking is not functional.
 */
#ifndef LIBSLIRP_H
#define LIBSLIRP_H

#include <stdint.h>

typedef struct Slirp Slirp;

typedef void (*SlirpTimerCb)(void *opaque);

typedef struct SlirpConfig {
    uint32_t if_mtu;
    uint32_t if_mru;
    // Minimal fields to compile
} SlirpConfig;

typedef struct SlirpCb {
    // empty
} SlirpCb;

// Minimal API stubs
#ifdef __cplusplus
extern "C" {
#endif

// None of these are called since SlirpEthernetConnection::Initialize returns false

#ifdef __cplusplus
}
#endif

#endif /* LIBSLIRP_H */
