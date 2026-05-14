#include "libcaesar.h"
#include <sys/mman.h>  // для mmap/mprotect
#include <stdio.h>
#include <string.h>

// Вместо глобального ключа используем защищённую область памяти
static void* secure_key_ptr = NULL;
static size_t key_size = sizeof(char);

/**
 * Инициализирует защищённую область памяти для ключа с помощью mmap.
 * Используются права доступа PROT_READ | PROT_WRITE для записи ключа.
 * Возвращает 0 при успехе, -1 при ошибке.
 */
int init_secure_key() {
    secure_key_ptr = mmap(
        NULL,              // адрес выбирается ядром
        key_size,          // размер области
        PROT_READ | PROT_WRITE, // права доступа
        MAP_PRIVATE | MAP_ANONYMOUS, // флаги mmap
        -1,               // FD не нужен для анонимной карты
        0                 // смещение
    );
    if (secure_key_ptr == MAP_FAILED) {
        fprintf(stderr, "Ошибка при выделении памяти для ключа: %m\n");
        return -1;
    }
    return 0;
}

/**
 * Устанавливает ключ шифрования в защищённую область памяти.
 * @param key - значение ключа (char)
 */
void set_key(char key) {
    if (secure_key_ptr == NULL) {
        if (init_secure_key() < 0) return; // пытаемся инициализировать
    }
    *(char*)secure_key_ptr = key; // записываем ключ
}

/**
 * Получает текущий ключ из защищённой области памяти.
 * @return значение ключа (char)
 */
char get_key() {
    if (secure_key_ptr == NULL) return 0;
    return *(char*)secure_key_ptr;
}

/**
 * Шифрует/дешифрует данные с использованием XOR и защищённого ключа.
 * @param src - указатель на исходные данные
 * @param dst - указатель на буфер для результата
 * @param len - длина данных
 */
void caesar(void* src, void* dst, int len) {
    unsigned char* s = (unsigned char*)src;
    unsigned char* d = (unsigned char*)dst;
    char key = get_key(); // получаем ключ из защищённой области

    for (int i = 0; i < len; i++) {
        d[i] = s[i] ^ key;
    }
}

/**
 * Очищает (затирает) защищённую область памяти с ключом и делает её недоступной.
 * Использует mprotect с PROT_NONE для запрета доступа к странице памяти.
 */
void clear_key() {
    if (secure_key_ptr != NULL) {
        // Затираем ключ перед отключением доступа
        memset(secure_key_ptr, 0, key_size);

        // Делаем страницу памяти недоступной (PROT_NONE)
        if (mprotect(secure_key_ptr, key_size, PROT_NONE) == -1) {
            fprintf(stderr, "Ошибка при защите памяти с ключом: %m\n");
        }

        // Отвязываем область памяти
        munmap(secure_key_ptr, key_size);
        secure_key_ptr = NULL;
    }
}
