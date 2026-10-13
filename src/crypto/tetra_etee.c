#include <dsd-neo/crypto/tetra_etee.h>

#include <tetraetee/etee.h>

int
tetra_etee_library_ready(void) {
    return get_alg_properties(ETEE_ALG_AES56) != 0 && get_alg_properties(ETEE_ALG_AES128) != 0
           && get_alg_properties(ETEE_ALG_AES256) != 0 && get_alg_properties(ETEE_ALG_IDEA) != 0;
}
