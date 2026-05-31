#include "libcaesar.h"
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

void secure_memset(void* ptr, int value, size_t size) {
    volatile uint8_t* p = (volatile uint8_t*)ptr;
    while (size--) *p++ = (uint8_t)value;
}

static void* allocate_secure_region(size_t size, int prot) {
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;
    size_t aligned = ((size + ps - 1) / ps) * ps;
    if (aligned == 0) aligned = (size_t)ps;
    
    void* ptr = mmap(NULL, aligned, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        return NULL;
    }
    return ptr;
}

static int toggle_protection(void* region, size_t size, int prot) {
    return mprotect(region, size, prot);
}

secure_rc4_context* init_secure_rc4(const uint8_t* key, size_t key_len, const uint8_t* salt) {
    if (!key || key_len == 0 || key_len > RC4_KEY_MAX_SIZE || !salt) return NULL;

    secure_rc4_context* ctx = calloc(1, sizeof(secure_rc4_context));
    if (!ctx) return NULL;
    pthread_mutex_init(&ctx->state_mutex, NULL);

    // Ключ
    ctx->key_region = allocate_secure_region(key_len, PROT_READ | PROT_WRITE);
    if (!ctx->key_region) goto fail;
    ctx->key_region_size = ((key_len + sysconf(_SC_PAGESIZE) - 1) / sysconf(_SC_PAGESIZE)) * sysconf(_SC_PAGESIZE);
    memcpy(ctx->key_region, key, key_len);
    ctx->key_len = key_len;
    // Скрываем ключ после копирования
    if (toggle_protection(ctx->key_region, ctx->key_region_size, PROT_NONE) != 0) {
        fprintf(stderr, "mprotect(key, NONE) failed: %s\n", strerror(errno));
        goto fail;
    }

    memcpy(ctx->salt, salt, SALT_SIZE);

    // Состояние
    ctx->state_region = allocate_secure_region(RC4_STATE_SIZE + 2, PROT_READ | PROT_WRITE);
    if (!ctx->state_region) goto fail;
    ctx->state_region_size = ((RC4_STATE_SIZE + 2 + sysconf(_SC_PAGESIZE) - 1) / sysconf(_SC_PAGESIZE)) * sysconf(_SC_PAGESIZE);
    
    // KSA
    uint8_t* S = ctx->state_region;
    for (int i = 0; i < RC4_STATE_SIZE; i++) S[i] = (uint8_t)i;

    uint8_t* tmp_key = (uint8_t*)ctx->key_region;
    if (toggle_protection(ctx->key_region, ctx->key_region_size, PROT_READ) != 0) { // Временно открываем ключ
        fprintf(stderr, "mprotect(key, READ) failed: %s\n", strerror(errno));
        goto fail;
    }
    size_t j = 0;
    for (size_t i = 0; i < RC4_STATE_SIZE; i++) {
        j = (j + S[i] + tmp_key[i % ctx->key_len]) % RC4_STATE_SIZE;
        uint8_t t = S[i]; S[i] = S[j]; S[j] = t;
    }
    if (toggle_protection(ctx->key_region, ctx->key_region_size, PROT_NONE) != 0) { // Закрываем
        fprintf(stderr, "mprotect(key, NONE) after KSA failed: %s\n", strerror(errno));
        goto fail;
    }

    ctx->state_region[RC4_STATE_SIZE] = 0; // i
    ctx->state_region[RC4_STATE_SIZE + 1] = 0; // j
    if (toggle_protection(ctx->state_region, ctx->state_region_size, PROT_NONE) != 0) { // Скрываем состояние
        fprintf(stderr, "mprotect(state, NONE) failed: %s\n", strerror(errno));
        goto fail;
    }

    return ctx;

fail:
    clear_secure_rc4(ctx);
    return NULL;
}

int rc4_crypt(secure_rc4_context* ctx, const void* src, void* dst, size_t len) {
    if (!ctx || !src || !dst || len == 0) return -1;

    pthread_mutex_lock(&ctx->state_mutex);
    if (toggle_protection(ctx->state_region, ctx->state_region_size, PROT_READ | PROT_WRITE) != 0) {
        pthread_mutex_unlock(&ctx->state_mutex);
        return -1;
    }

    const uint8_t* s = (const uint8_t*)src;
    uint8_t* d = (uint8_t*)dst;
    uint8_t* S = ctx->state_region;
    uint8_t a = ctx->state_region[RC4_STATE_SIZE];
    uint8_t b = ctx->state_region[RC4_STATE_SIZE + 1];

    for (size_t n = 0; n < len; n++) {
        a = (a + 1) & 0xFF;
        b = (b + S[a]) & 0xFF;
        uint8_t t = S[a]; S[a] = S[b]; S[b] = t;
        d[n] = s[n] ^ S[(S[a] + S[b]) & 0xFF];
    }

    ctx->state_region[RC4_STATE_SIZE] = a;
    ctx->state_region[RC4_STATE_SIZE + 1] = b;
    if (toggle_protection(ctx->state_region, ctx->state_region_size, PROT_NONE) != 0) {
        /* Состояние уже обновлено, ошибка mprotect некритична для корректности */
    }
    pthread_mutex_unlock(&ctx->state_mutex);
    return 0;
}

const uint8_t* get_salt(const secure_rc4_context* ctx) {
    return ctx ? ctx->salt : NULL;
}

void clear_secure_rc4(secure_rc4_context* ctx) {
    if (!ctx) return;
    
    if (ctx->key_region) {
        if (mprotect(ctx->key_region, ctx->key_region_size, PROT_READ | PROT_WRITE) == 0)
            secure_memset(ctx->key_region, 0, ctx->key_len);
        munmap(ctx->key_region, ctx->key_region_size);
    }
    if (ctx->state_region) {
        if (mprotect(ctx->state_region, ctx->state_region_size, PROT_READ | PROT_WRITE) == 0)
            secure_memset(ctx->state_region, 0, RC4_STATE_SIZE + 2);
        munmap(ctx->state_region, ctx->state_region_size);
    }
    pthread_mutex_destroy(&ctx->state_mutex);
    secure_memset(ctx, 0, sizeof(*ctx));
    free(ctx);
}
