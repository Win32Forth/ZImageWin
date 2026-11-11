/* Port of ZImager/Document/Blake3.swift. Empty input is
   af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262. */
#include "blake3.h"

#include <windows.h>

#include <stdint.h>
#include <string.h>

static const uint32_t k_iv[8] = {
    0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
    0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u
};

static const int k_perm[16] = { 2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8 };

enum {
    ZI_B3_BLOCK = 64,
    ZI_B3_CHUNK = 1024,
    ZI_B3_CHUNK_START = 1,
    ZI_B3_CHUNK_END = 2,
    ZI_B3_PARENT = 4,
    ZI_B3_ROOT = 8,
    ZI_B3_STACK = 54
};

static uint32_t zi_rotr(uint32_t value, int count) {
    return (value >> count) | (value << (32 - count));
}

static void zi_mix(uint32_t state[16], int a, int b, int c, int d, uint32_t mx, uint32_t my) {
    state[a] = state[a] + state[b] + mx;
    state[d] = zi_rotr(state[d] ^ state[a], 16);
    state[c] = state[c] + state[d];
    state[b] = zi_rotr(state[b] ^ state[c], 12);
    state[a] = state[a] + state[b] + my;
    state[d] = zi_rotr(state[d] ^ state[a], 8);
    state[c] = state[c] + state[d];
    state[b] = zi_rotr(state[b] ^ state[c], 7);
}

static void zi_compress(const uint32_t cv[8], const uint32_t block[16], uint64_t counter,
                        uint32_t block_len, uint32_t flags, uint32_t out[16]) {
    uint32_t state[16];
    uint32_t message[16];
    int round;
    int i;

    for (i = 0; i < 8; i++) {
        state[i] = cv[i];
    }
    state[8] = k_iv[0];
    state[9] = k_iv[1];
    state[10] = k_iv[2];
    state[11] = k_iv[3];
    state[12] = (uint32_t)counter;
    state[13] = (uint32_t)(counter >> 32);
    state[14] = block_len;
    state[15] = flags;
    memcpy(message, block, sizeof message);
    for (round = 0; round < 7; round++) {
        uint32_t next[16];

        zi_mix(state, 0, 4, 8, 12, message[0], message[1]);
        zi_mix(state, 1, 5, 9, 13, message[2], message[3]);
        zi_mix(state, 2, 6, 10, 14, message[4], message[5]);
        zi_mix(state, 3, 7, 11, 15, message[6], message[7]);
        zi_mix(state, 0, 5, 10, 15, message[8], message[9]);
        zi_mix(state, 1, 6, 11, 12, message[10], message[11]);
        zi_mix(state, 2, 7, 8, 13, message[12], message[13]);
        zi_mix(state, 3, 4, 9, 14, message[14], message[15]);
        for (i = 0; i < 16; i++) {
            next[i] = message[k_perm[i]];
        }
        memcpy(message, next, sizeof message);
    }
    for (i = 0; i < 8; i++) {
        state[i] ^= state[i + 8];
        state[i + 8] ^= cv[i];
    }
    memcpy(out, state, sizeof state);
}

static void zi_words(const unsigned char block[ZI_B3_BLOCK], uint32_t out[16]) {
    int i;

    for (i = 0; i < 16; i++) {
        int offset = i * 4;

        out[i] = (uint32_t)block[offset] |
                 ((uint32_t)block[offset + 1] << 8) |
                 ((uint32_t)block[offset + 2] << 16) |
                 ((uint32_t)block[offset + 3] << 24);
    }
}

typedef struct ZiChunk {
    uint32_t cv[8];
    uint64_t counter;
    unsigned char block[ZI_B3_BLOCK];
    int block_len;
    int blocks;
} ZiChunk;

typedef struct ZiHasher {
    ZiChunk chunk;
    uint32_t stack[ZI_B3_STACK][8];
    int depth;
} ZiHasher;

static void zi_chunk_init(ZiChunk *chunk, uint64_t counter) {
    memset(chunk, 0, sizeof *chunk);
    memcpy(chunk->cv, k_iv, sizeof k_iv);
    chunk->counter = counter;
}

static int zi_chunk_count(const ZiChunk *chunk) {
    return ZI_B3_BLOCK * chunk->blocks + chunk->block_len;
}

static void zi_chunk_update(ZiChunk *chunk, const unsigned char *data, size_t length) {
    size_t offset = 0;

    while (offset < length) {
        size_t room;
        size_t take;

        if (chunk->block_len == ZI_B3_BLOCK) {
            uint32_t block[16];
            uint32_t wide[16];
            uint32_t flags = chunk->blocks == 0 ? ZI_B3_CHUNK_START : 0;

            zi_words(chunk->block, block);
            zi_compress(chunk->cv, block, chunk->counter, ZI_B3_BLOCK, flags, wide);
            memcpy(chunk->cv, wide, 8 * sizeof(uint32_t));
            chunk->blocks++;
            memset(chunk->block, 0, sizeof chunk->block);
            chunk->block_len = 0;
        }
        room = (size_t)(ZI_B3_BLOCK - chunk->block_len);
        take = length - offset;
        if (take > room) {
            take = room;
        }
        memcpy(chunk->block + chunk->block_len, data + offset, take);
        chunk->block_len += (int)take;
        offset += take;
    }
}

static void zi_chunk_output(const ZiChunk *chunk, uint32_t cv[8], uint32_t block[16], uint32_t *block_len, uint32_t *flags) {
    uint32_t end_flags = ZI_B3_CHUNK_END;

    if (chunk->blocks == 0) {
        end_flags |= ZI_B3_CHUNK_START;
    }
    memcpy(cv, chunk->cv, 8 * sizeof(uint32_t));
    zi_words(chunk->block, block);
    *block_len = (uint32_t)chunk->block_len;
    *flags = end_flags;
}

static void zi_parent(const uint32_t left[8], const uint32_t right[8], uint32_t cv[8], uint32_t block[16]) {
    int i;

    memcpy(cv, k_iv, sizeof k_iv);
    for (i = 0; i < 8; i++) {
        block[i] = left[i];
        block[i + 8] = right[i];
    }
}

static void zi_add_chunk(ZiHasher *hasher, const uint32_t cv[8], uint64_t total) {
    uint32_t value[8];

    memcpy(value, cv, sizeof value);
    while ((total & 1ull) == 0 && hasher->depth > 0) {
        uint32_t parent_cv[8];
        uint32_t block[16];
        uint32_t wide[16];

        hasher->depth--;
        zi_parent(hasher->stack[hasher->depth], value, parent_cv, block);
        zi_compress(parent_cv, block, 0, ZI_B3_BLOCK, ZI_B3_PARENT, wide);
        memcpy(value, wide, sizeof value);
        total >>= 1;
    }
    if (hasher->depth < ZI_B3_STACK) {
        memcpy(hasher->stack[hasher->depth], value, sizeof value);
        hasher->depth++;
    }
}

static void zi_hasher_init(ZiHasher *hasher) {
    memset(hasher, 0, sizeof *hasher);
    zi_chunk_init(&hasher->chunk, 0);
}

static void zi_hasher_update(ZiHasher *hasher, const unsigned char *data, size_t length) {
    size_t offset = 0;

    while (offset < length) {
        size_t room;
        size_t take;

        if (zi_chunk_count(&hasher->chunk) == ZI_B3_CHUNK) {
            uint32_t cv[8];
            uint32_t block[16];
            uint32_t wide[16];
            uint32_t block_len = 0;
            uint32_t flags = 0;
            uint64_t total;

            zi_chunk_output(&hasher->chunk, cv, block, &block_len, &flags);
            zi_compress(cv, block, hasher->chunk.counter, block_len, flags, wide);
            total = hasher->chunk.counter + 1;
            zi_add_chunk(hasher, wide, total);
            zi_chunk_init(&hasher->chunk, total);
        }
        room = (size_t)(ZI_B3_CHUNK - zi_chunk_count(&hasher->chunk));
        take = length - offset;
        if (take > room) {
            take = room;
        }
        zi_chunk_update(&hasher->chunk, data + offset, take);
        offset += take;
    }
}

static void zi_hasher_final(const ZiHasher *hasher, unsigned char out[32]) {
    uint32_t cv[8];
    uint32_t block[16];
    uint32_t block_len = 0;
    uint32_t flags = 0;
    uint64_t counter;
    uint32_t wide[16];
    int remaining;
    int i;

    zi_chunk_output(&hasher->chunk, cv, block, &block_len, &flags);
    counter = hasher->chunk.counter;
    remaining = hasher->depth;
    while (remaining > 0) {
        uint32_t chaining[8];
        uint32_t parent_cv[8];
        uint32_t parent_block[16];

        remaining--;
        zi_compress(cv, block, counter, block_len, flags, wide);
        memcpy(chaining, wide, sizeof chaining);
        zi_parent(hasher->stack[remaining], chaining, parent_cv, parent_block);
        memcpy(cv, parent_cv, sizeof cv);
        memcpy(block, parent_block, sizeof block);
        block_len = ZI_B3_BLOCK;
        flags = ZI_B3_PARENT;
        counter = 0;
    }
    zi_compress(cv, block, 0, block_len, flags | ZI_B3_ROOT, wide);
    for (i = 0; i < 8; i++) {
        out[i * 4] = (unsigned char)wide[i];
        out[i * 4 + 1] = (unsigned char)(wide[i] >> 8);
        out[i * 4 + 2] = (unsigned char)(wide[i] >> 16);
        out[i * 4 + 3] = (unsigned char)(wide[i] >> 24);
    }
}

int zi_blake3_hash(const void *data, size_t length, unsigned char out[32]) {
    ZiHasher hasher;

    if (!out) {
        return 0;
    }
    zi_hasher_init(&hasher);
    if (data && length > 0) {
        zi_hasher_update(&hasher, (const unsigned char *)data, length);
    }
    zi_hasher_final(&hasher, out);
    return 1;
}

int zi_blake3_file(const wchar_t *path, unsigned char out[32], unsigned long long *size) {
    HANDLE file;
    LARGE_INTEGER bytes;
    ZiHasher hasher;
    unsigned char buffer[1 << 16];

    if (size) {
        *size = 0;
    }
    if (!path || !path[0] || !out) {
        return 0;
    }
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }
    if (!GetFileSizeEx(file, &bytes) || bytes.QuadPart < 0) {
        CloseHandle(file);
        return 0;
    }
    zi_hasher_init(&hasher);
    for (;;) {
        DWORD got = 0;

        if (!ReadFile(file, buffer, sizeof buffer, &got, NULL)) {
            CloseHandle(file);
            return 0;
        }
        if (got == 0) {
            break;
        }
        zi_hasher_update(&hasher, buffer, got);
    }
    CloseHandle(file);
    zi_hasher_final(&hasher, out);
    if (size) {
        *size = (unsigned long long)bytes.QuadPart;
    }
    return 1;
}

void zi_blake3_hex(const unsigned char in[32], char out[65]) {
    static const char *k_hex = "0123456789abcdef";
    int i;

    for (i = 0; i < 32; i++) {
        out[i * 2] = k_hex[in[i] >> 4];
        out[i * 2 + 1] = k_hex[in[i] & 15];
    }
    out[64] = '\0';
}

int zi_blake3_from_hex(const char *text, unsigned char out[32]) {
    int i;

    if (!text || !out) {
        return 0;
    }
    for (i = 0; i < 32; i++) {
        char hi = text[i * 2];
        char lo = text[i * 2 + 1];
        int high;
        int low;

        if (hi >= '0' && hi <= '9') {
            high = hi - '0';
        } else if (hi >= 'a' && hi <= 'f') {
            high = hi - 'a' + 10;
        } else if (hi >= 'A' && hi <= 'F') {
            high = hi - 'A' + 10;
        } else {
            return 0;
        }
        if (lo >= '0' && lo <= '9') {
            low = lo - '0';
        } else if (lo >= 'a' && lo <= 'f') {
            low = lo - 'a' + 10;
        } else if (lo >= 'A' && lo <= 'F') {
            low = lo - 'A' + 10;
        } else {
            return 0;
        }
        out[i] = (unsigned char)((high << 4) | low);
    }
    return 1;
}
