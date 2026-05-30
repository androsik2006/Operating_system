#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#include <stdint.h>
#include "libcaesar.h"

void secure_memset(void* ptr, int value, size_t size) {
    volatile uint8_t* p = (volatile uint8_t*)ptr;
    while (size--) *p++ = (uint8_t)value;
}

int hex_to_bytes(const char* hex, uint8_t* bytes, size_t max_len) {
    size_t len = strlen(hex) / 2;
    if (len > max_len) len = max_len;
    for (size_t i = 0; i < len; i++) sscanf(hex + 2 * i, "%2hhx", &bytes[i]);
    return len;
}

int main(int argc, char* argv[]) {
    if (argc != 6) {
        fprintf(stderr, "Использование: %s <lib.so> <key_hex> <salt_hex> <input> <output>\n", argv[0]);
        return 1;
    }

    uint8_t key[RC4_KEY_MAX_SIZE], salt[SALT_SIZE];
    size_t key_len = hex_to_bytes(argv[2], key, RC4_KEY_MAX_SIZE);
    hex_to_bytes(argv[3], salt, SALT_SIZE);

    void* handle = dlopen(argv[1], RTLD_LAZY);
    if (!handle) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }

    secure_rc4_context* (*init)(const uint8_t*, size_t, const uint8_t*) = dlsym(handle, "init_secure_rc4");
    int (*crypt)(secure_rc4_context*, const void*, void*, size_t) = dlsym(handle, "rc4_crypt");
    void (*clear)(secure_rc4_context*) = dlsym(handle, "clear_secure_rc4");
    if (!init || !crypt || !clear) { fprintf(stderr, "dlsym: %s\n", dlerror()); dlclose(handle); return 1; }

    FILE* fin = fopen(argv[4], "rb");
    if (!fin) { perror("fopen input"); dlclose(handle); return 1; }
    fseek(fin, 0, SEEK_END);
    long len = ftell(fin);
    fseek(fin, 0, SEEK_SET);
    void* buffer = malloc(len);
    if (!buffer) { perror("malloc"); fclose(fin); dlclose(handle); return 1; }
    fread(buffer, 1, len, fin);
    fclose(fin);

    secure_rc4_context* ctx = init(key, key_len, salt);
    if (!ctx) { fprintf(stderr, "Init failed\n"); free(buffer); dlclose(handle); return 1; }

    if (crypt(ctx, buffer, buffer, len) != 0) {
        fprintf(stderr, "Crypt failed\n"); clear(ctx); secure_memset(buffer, 0, len); free(buffer); dlclose(handle); return 1;
    }

    FILE* fout = fopen(argv[5], "wb");
    if (!fout) { perror("fopen output"); clear(ctx); secure_memset(buffer, 0, len); free(buffer); dlclose(handle); return 1; }
    fwrite(buffer, 1, len, fout);
    fclose(fout);

    clear(ctx);
    secure_memset(buffer, 0, len);
    free(buffer);
    dlclose(handle);
    printf("Файл успешно обработан: %s -> %s\n", argv[4], argv[5]);
    return 0;
}
