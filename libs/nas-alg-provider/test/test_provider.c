/* TEST-ONLY provider for the loader tests (ADR-0479). This is NOT SNOW 3G and implements no
 * 3GPP algorithm: "eea1" XORs the input with the key bytes repeated, "eia1" is a trivial byte
 * checksum. It exists solely so the loader's fail-closed gates can be exercised without any
 * restricted material. The self-test "vector file" is a list of lines: "ok" counts as a pass,
 * anything else as a fail. */
#include "nas_alg_provider/nas_alg_provider.h"

#include <stdio.h>
#include <string.h>

static const nasalg_info kInfo = {NASALG_ABI_VERSION, NASALG_ALG_128_EEA1 | NASALG_ALG_128_EIA1,
                                  "TEST-ONLY-NOT-SNOW3G", "test-package-none", 1u};

const nasalg_info* nasalg_describe(void) { return &kInfo; }

int nasalg_eea1(const uint8_t key[16], uint32_t count, uint8_t bearer, uint8_t direction,
                const uint8_t* in, uint8_t* out, uint32_t length_bits) {
    (void)count; (void)bearer; (void)direction;
    if (key == NULL || (in == NULL && length_bits != 0) || (out == NULL && length_bits != 0)) {
        return NASALG_E_BAD_ARG;
    }
    uint32_t n = (length_bits + 7u) / 8u;
    for (uint32_t i = 0; i < n; ++i) {
        out[i] = (uint8_t)(in[i] ^ key[i % 16u]);
    }
    if ((length_bits % 8u) != 0u && n > 0u) {
        out[n - 1u] &= (uint8_t)(0xFFu << (8u - (length_bits % 8u)));
    }
    return 0;
}

int nasalg_eia1(const uint8_t key[16], uint32_t count, uint32_t fresh, uint8_t direction,
                const uint8_t* msg, uint32_t length_bits, uint8_t mac[4]) {
    (void)count; (void)fresh; (void)direction;
    if (key == NULL || mac == NULL || (msg == NULL && length_bits != 0)) {
        return NASALG_E_BAD_ARG;
    }
    uint8_t acc = 0;
    for (uint32_t i = 0; i < (length_bits + 7u) / 8u; ++i) {
        acc = (uint8_t)(acc + msg[i]);
    }
    memset(mac, acc, 4);
    return 0;
}

int nasalg_selftest(const char* vectors_path, uint32_t* passed, uint32_t* total) {
    FILE* f = fopen(vectors_path, "r");
    if (f == NULL) {
        return NASALG_E_BAD_ARG;
    }
    char line[64];
    *passed = 0;
    *total = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        ++*total;
        if (strncmp(line, "ok", 2) == 0) {
            ++*passed;
        }
    }
    fclose(f);
    return 0;
}
