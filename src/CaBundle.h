#pragma once
#include <stdint.h>
#include <stddef.h>

// ESP-IDF's embedded Mozilla root-CA bundle (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y). Used to verify
// the portal's certificate for the API and the Reverb WebSocket instead of setInsecure().
extern "C" const uint8_t x509_crt_imported_bundle_bin_start[] asm("_binary_x509_crt_bundle_start");
extern "C" const uint8_t x509_crt_imported_bundle_bin_end[] asm("_binary_x509_crt_bundle_end");

inline const uint8_t *caBundleStart() { return x509_crt_imported_bundle_bin_start; }
inline size_t caBundleSize() { return (size_t)(x509_crt_imported_bundle_bin_end - x509_crt_imported_bundle_bin_start); }
