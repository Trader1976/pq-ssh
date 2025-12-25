#include <stddef.h>
#include <stdint.h>
#include <sodium.h>

/* signature expected by vendor code */
int qgp_randombytes(uint8_t *out, size_t outlen)
{
    if (!out) return -1;

    /* sodium_init is cheap; but ideally call once at app startup */
    if (sodium_init() < 0) return -1;

    randombytes_buf(out, outlen);
    return 0;
}
