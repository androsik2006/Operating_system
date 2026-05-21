#ifndef LIBCAESAR_H
#define LIBCAESAR_H

#include <stddef.h>  // для size_t
#include <stdint.h> // для uint8_t

// Константы
#define RC4_KEY_MAX_SIZE 32      // Максимальный размер ключа RC4
#define SALT_SIZE 16            // Размер соли (16 байт по ТЗ)
#define RC4_STATE_SIZE 256      // Размер внутреннего состояния RC4 (256 байт)

// Структура для хранения защищённого состояния RC4
typedef struct {
    uint8_t* key;           // Указатель на защищённый ключ
    size_t key_len;         // Длина ключа
    uint8_t salt[SALT_SIZE]; // Соль (16 байт)
    uint8_t* state;          // Защищённое внутреннее состояние (256 байт S‑блок)
    uint8_t i, j;          // Текущие индексы RC4
} secure_rc4_context;

// Инициализация защищённой области памяти для RC4
// Возвращает указатель на контекст или NULL при ошибке
secure_rc4_context* init_secure_rc4(const uint8_t* key, size_t key_len, const uint8_t* salt);

// Шифрование/дешифрование данных с использованием RC4
// Использует защищённое состояние и ключ
// Возвращает 0 при успехе, -1 при ошибке
int rc4_crypt(secure_rc4_context* ctx, const void* src, void* dst, size_t len);

// Получение соли из контекста
const uint8_t* get_salt(const secure_rc4_context* ctx);

// Очистка защищённой памяти (затирание и освобождение)
void clear_secure_rc4(secure_rc4_context* ctx);

// Вспомогательные функции для работы с памятью
// Безопасное затирание памяти (не оптимизируется компилятором)
void secure_memset(void* ptr, int value, size_t size);

#endif // LIBCAESAR_H
