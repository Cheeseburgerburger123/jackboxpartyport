// Changes to mbedTLS's default configuration for the Wii U build (MBEDTLS_USER_CONFIG_FILE).
// Sockets are ours (net_wiiu.c), randomness comes from mbedtls_hardware_poll there, and only TLS 1.2
// is built: every server Jackbox talks to speaks it, and 1.3 would need the PSA crypto setup too.
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_SSL_PROTO_TLS1_3
#undef MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
// newlib on the console doesn't announce POSIX clocks: net_wiiu.c provides the millisecond clock.
#define MBEDTLS_PLATFORM_MS_TIME_ALT
