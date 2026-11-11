/* Unkeyed BLAKE3, 32 bytes. The Mac document hashes the original with this. */
#ifndef ZIMAGEWIN_BLAKE3_H
#define ZIMAGEWIN_BLAKE3_H

#include <stddef.h>

int zi_blake3_hash(const void *data, size_t length, unsigned char out[32]);
/* Reads the whole file. *size receives the byte count. Returns 0 if the file cannot be read. */
int zi_blake3_file(const wchar_t *path, unsigned char out[32], unsigned long long *size);
void zi_blake3_hex(const unsigned char in[32], char out[65]);
int zi_blake3_from_hex(const char *text, unsigned char out[32]);

#endif
