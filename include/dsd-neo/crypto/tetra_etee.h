/* End-to-end TETRA crypto via the tetraetee vcpkg port.
 * Callers supply the key. This wrapper does not search for one. */
#ifndef DSD_NEO_CRYPTO_TETRA_ETEE_H
#define DSD_NEO_CRYPTO_TETRA_ETEE_H

#include <stdint.h>

/* 1 when the library recognises AES-56, AES-128, AES-256 and IDEA. */
int tetra_etee_library_ready(void);

#endif /* DSD_NEO_CRYPTO_TETRA_ETEE_H */
