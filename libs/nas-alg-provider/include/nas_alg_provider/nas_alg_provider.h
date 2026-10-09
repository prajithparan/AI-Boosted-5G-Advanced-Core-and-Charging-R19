/* nas_alg_provider.h -- stable C ABI between the AMF and an operator-supplied NAS algorithm
 * provider library (ADR-0479, snow3g/docs/framework-design.md). THIS FILE CONTAINS NO ALGORITHM
 * AND NO SAGE-DERIVED CONTENT; the names are this project's own. A provider is a shared library
 * the licensed operator builds locally. All functions return 0 on success, a negative
 * NASALG_E_* value otherwise. No allocation crosses the boundary; lengths are explicit; the
 * library keeps no global mutable state visible to the caller.
 */
#ifndef NAS_ALG_PROVIDER_H
#define NAS_ALG_PROVIDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NASALG_ABI_VERSION 1u

/* Bits of nasalg_info.algorithms. */
#define NASALG_ALG_128_EEA1 0x1u /* ciphering, algorithm identity 1 (TS 33.501 5.11.1) */
#define NASALG_ALG_128_EIA1 0x2u /* integrity,  algorithm identity 1 (TS 33.501 5.11.1) */

#define NASALG_E_BAD_ARG (-1)
#define NASALG_E_UNSUPPORTED (-2)
#define NASALG_E_INTERNAL (-3)

typedef struct nasalg_info {
    uint32_t abi_version;       /* must equal NASALG_ABI_VERSION */
    uint32_t algorithms;        /* OR of NASALG_ALG_* this library implements */
    const char* provider_id;    /* free text, e.g. build identity; logged, never parsed */
    const char* package_id;     /* operator-side package fingerprint id (traceability) */
    uint32_t test_only;         /* non-zero: NOT a real algorithm; refused unless explicitly allowed */
} nasalg_info;

/* Mandatory exports (resolved by dlsym with exactly these names). */
const nasalg_info* nasalg_describe(void);

/* 128-EEA1 style keystream XOR. key: 16 bytes. direction: 0 uplink, 1 downlink. in/out may alias
 * exactly (in == out) or not overlap. length_bits may be any value; bytes beyond ceil(bits/8)
 * are neither read nor written, and unused low bits of the last output byte are zeroed. */
int nasalg_eea1(const uint8_t key[16], uint32_t count, uint8_t bearer, uint8_t direction,
                const uint8_t* in, uint8_t* out, uint32_t length_bits);

/* 128-EIA1 style MAC. mac: 4 bytes. */
int nasalg_eia1(const uint8_t key[16], uint32_t count, uint32_t fresh, uint8_t direction,
                const uint8_t* msg, uint32_t length_bits, uint8_t mac[4]);

/* Known-answer self-test against the operator's own vector set at vectors_path. Returns 0 only
 * if it ran; *passed / *total report the result. The AMF treats passed != total, or total == 0,
 * as failure. */
int nasalg_selftest(const char* vectors_path, uint32_t* passed, uint32_t* total);

#ifdef __cplusplus
}
#endif
#endif
