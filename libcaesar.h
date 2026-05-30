#ifndef LIBCAESAR_H
#define LIBCAESAR_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#define RC4_KEY_MAX_SIZE 32
#define SALT_SIZE 16
#define RC4_STATE_SIZE 256

typedef struct {
    uint8_t* key_region;          // Защищённая область ключа (mmap)
    size_t key_region_size;       // Размер выровненной области ключа
    size_t key_len;               // Фактическая длина ключа
    uint8_t salt[SALT_SIZE];      // Соль
    uint8_t* state_region;        // Защищённая область состояния (S-блок + индексы)
    size_t state_region_size;     // Размер выровненной области состояния
    pthread_mutex_t state_mutex;  // Мьютекс для защиты конкурентного доступа
    uint8_t i, j;                 // Индексы PRGA (вынесены для удобства)
} secure_rc4_context;

// Инициализация защищённого контекста RC4
secure_rc4_context* init_secure_rc4(const uint8_t* key, size_t key_len, const uint8_t* salt);

// Шифрование/дешифрование (RC4 симметричен)
int rc4_crypt(secure_rc4_context* ctx, const void* src, void* dst, size_t len);

// Получение соли
const uint8_t* get_salt(const secure_rc4_context* ctx);

// Безопасное освобождение и затирание памяти
void clear_secure_rc4(secure_rc4_context* ctx);

// Безопасное затирание (не оптимизируется компилятором)
void secure_memset(void* ptr, int value, size_t size);

#endif // LIBCAESAR_H
