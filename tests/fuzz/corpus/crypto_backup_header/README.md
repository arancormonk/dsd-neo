# Backup Header Fuzz Corpus

Seeds for `dsd_backup_parse_header` (`tests/fuzz/fuzz_crypto_backup_header.c`).

- `golden.bin`: the sealed golden vector from `tests/crypto/test_backup_seal.c` (log2(N) 14, r 8, p 1, 33-byte
  payload), a complete valid envelope.
- `truncated_header.bin`: its first 40 bytes, cut inside the nonce.
