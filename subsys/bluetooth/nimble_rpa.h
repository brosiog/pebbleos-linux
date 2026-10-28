/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <mbedtls/aes.h>

// Bluetooth ah(): NimBLE stores the IRK and address octets least significant first.
static inline bool nimble_rpa_matches_irk(const uint8_t address[6], const uint8_t irk[16]) {
  if ((address[5] & 0xc0) != 0x40) {
    return false;
  }
  uint8_t key[16];
  uint8_t plaintext[16] = {0};
  uint8_t encrypted[16];
  for (unsigned i = 0; i < 16; ++i) {
    key[i] = irk[15 - i];
  }
  plaintext[13] = address[5];
  plaintext[14] = address[4];
  plaintext[15] = address[3];
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  int rc = mbedtls_aes_setkey_enc(&aes, key, 128);
  if (rc == 0) {
    rc = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, plaintext, encrypted);
  }
  mbedtls_aes_free(&aes);
  return rc == 0 && encrypted[15] == address[0] && encrypted[14] == address[1] &&
         encrypted[13] == address[2];
}
