#include "libcaesar.h"
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

// Безопасное затирание памяти — не оптимизируется компилятором
void secure_memset(void* ptr, int value, size_t size) {
    volatile uint8_t* p = (volatile uint8_t*)ptr;
    while (size--) {
        *p++ = (uint8_t)value;
    }
}

static void* allocate_secure_memory(size_t size) {
    void* ptr = mmap(
        NULL,
        size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0
    );
    if (ptr == MAP_FAILED) {
        fprintf(stderr, "Ошибка выделения защищённой памяти: %s\n", strerror(errno));
        return NULL;
    }
    return ptr;
}

secure_rc4_context* init_secure_rc4(const uint8_t* key, size_t key_len, const uint8_t* salt) {
    if (!key || key_len == 0 || key_len > RC4_KEY_MAX_SIZE || !salt) {
        return NULL;
    }

    secure_rc4_context* ctx = malloc(sizeof(secure_rc4_context));
    if (!ctx) return NULL;

    // Выделяем защищённую память для ключа
    ctx->key = allocate_secure_memory(key_len);
    if (!ctx->key) {
        free(ctx);
        return NULL;
    }
    memcpy(ctx->key, key, key_len);
    ctx->key_len = key_len;

    // Копируем соль (16 байт)
    memcpy(ctx->salt, salt, SALT_SIZE);

    // Выделяем защищённую память для состояния RC4 (256 байт)
    ctx->state = allocate_secure_memory(RC4_STATE_SIZE);
    if (!ctx->state) {
        clear_secure_rc4(ctx); // Очистка уже выделенных ресурсов
        return NULL;
    }

    // Инициализация S‑блока RC4
    for (int i = 0; i < RC4_STATE_SIZE; i++) {
        ctx->state[i] = (uint8_t)i;
    }

    // KSA (Key Scheduling Algorithm)
    uint8_t tmp;
    size_t j = 0;
    for (size_t i = 0; i < RC4_STATE_SIZE; i++) {
        j = (j + ctx->state[i] + ctx->key[i % ctx->key_len]) % RC4_STATE_SIZE;
        tmp = ctx->state[i];
        ctx->state[i] = ctx->state[j];
        ctx->state[j] = tmp;
    }

    ctx->i = 0;
    ctx->j = 0;

    return ctx;
}

int rc4_crypt(secure_rc4_context* ctx, const void* src, void* dst, size_t len) {
    if (!ctx || !src || !dst || len == 0) return -1;

    const uint8_t* s = (const uint8_t*)src;
    uint8_t* d = (uint8_t*)dst;
    uint8_t tmp;

    for (size_t n = 0; n < len; n++) {
        // PRGA (Pseudo-Random Generation Algorithm)
        ctx->i = (ctx->i + 1) % RC4_STATE_SIZE;
        ctx->j = (ctx->j + ctx->state[ctx->i]) % RC4_STATE_SIZE;

        tmp = ctx->state[ctx->i];
        ctx->state[ctx->i] = ctx->state[ctx->j];
        ctx->state[ctx->j] = tmp;

        uint8_t k = ctx->state[(ctx->state[ctx->i] + ctx->state[ctx->j]) % RC4_STATE_SIZE];
        d[n] = s[n] ^ k;
    }

    return 0;
}

const uint8_t* get_salt(const secure_rc4_context* ctx) {
    return ctx ? ctx->salt : NULL;
}

void clear_secure_rc4(secure_rc4_context* ctx) {
    if (!ctx) return;

    if (ctx->key) {
        secure_memset(ctx->key, 0, ctx->key_len);
        munmap(ctx->key, ctx->key_len);
    }

    if (ctx->state) {
        secure_memset(ctx->state, 0, RC4_STATE_SIZE);
        munmap(ctx->state, RC4_STATE_SIZE);
    }

    free(ctx);
}
