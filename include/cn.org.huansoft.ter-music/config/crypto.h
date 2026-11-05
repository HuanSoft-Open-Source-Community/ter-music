#ifndef CRYPTO_H
#define CRYPTO_H

#include <stddef.h>

void crypto_encrypt(const char *plaintext, char *hex_out, size_t hex_out_size);
void crypto_decrypt(const char *hex_in, char *plaintext_out, size_t plaintext_size);

#endif
