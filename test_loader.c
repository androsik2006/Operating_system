#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#include <stdint.h>  // для uint8_t

// Типы функций для RC4
typedef secure_rc4_context* (*init_secure_rc4_func)(const uint8_t*, size_t, const uint8_t*);
typedef int (*rc4_crypt_func)(secure_rc4_context*, const void*, void*, size_t);
typedef void (*clear_secure_rc4_func)(secure_rc4_context*);

// Безопасное затирание памяти
void secure_memset(void* ptr, int value, size_t size) {
    volatile uint8_t* p = (volatile uint8_t*)ptr;
    while (size--) {
        *p++ = (uint8_t)value;
    }
}

// Вспомогательная функция для преобразования hex-строки в байты
int hex_to_bytes(const char* hex, uint8_t* bytes, size_t max_len) {
    size_t len = strlen(hex) / 2;
    if (len > max_len) len = max_len;

    for (size_t i = 0; i < len; i++) {
        sscanf(hex + 2 * i, "%2hhx", &bytes[i]);
    }
    return len;
}

int main(int argc, char* argv[]) {
    if (argc != 6) {
        fprintf(stderr, "Использование: %s <путь_к_библиотеке> <ключ_hex> <соль_hex> <входной_файл> <выходной_файл>\n", argv[0]);
        fprintf(stderr, "Пример: %s ./libcaesar.so 0123456789ABCDEF0123456789ABCDEF 00112233445566778899AABBCCDDEEFF input.txt output.enc\n", argv[0]);
        return 1;
    }

    const char* lib_path = argv[1];
    const char* key_hex = argv[2];
    const char* salt_hex = argv[3];
    const char* in_path = argv[4];
    const char* out_path = argv[5];

    // Преобразование ключа и соли из hex
    uint8_t key[RC4_KEY_MAX_SIZE];
    size_t key_len = hex_to_bytes(key_hex, key, RC4_KEY_MAX_SIZE);
    uint8_t salt[SALT_SIZE];
    hex_to_bytes(salt_hex, salt, SALT_SIZE);

    // 1. Динамическая загрузка библиотеки
    void* handle = dlopen(lib_path, RTLD_LAZY);
    if (!handle) {
        fprintf(stderr, "Ошибка при загрузке библиотеки: %s\n", dlerror());
        return 1;
    }

    // 2. Получение адресов функций
    init_secure_rc4_func init_secure_rc4 = (init_secure_rc4_func)dlsym(handle, "init_secure_rc4");
    rc4_crypt_func rc4_crypt = (rc4_crypt_func)dlsym(handle, "rc4_crypt");
    clear_secure_rc4_func clear_secure_rc4 = (clear_secure_rc4_func)dlsym(handle, "clear_secure_rc4");

    if (!init_secure_rc4 || !rc4_crypt || !clear_secure_rc4) {
        fprintf(stderr, "Ошибка при поиске символов: %s\n", dlerror());
        dlclose(handle);
        return 1;
    }

    // 3. Чтение файла
    FILE* fin = fopen(in_path, "rb");
    if (!fin) {
        perror("fopen input");
        dlclose(handle);
        return 1;
    }

    fseek(fin, 0, SEEK_END);
    long len = ftell(fin);
    fseek(fin, 0, SEEK_SET);

    void* buffer = malloc(len);
    if (!buffer) {
        perror("malloc");
        fclose(fin);
        dlclose(handle);
        return 1;
    }

    fread(buffer, 1, len, fin);
    fclose(fin);

    // 4. Инициализация защищённой области памяти для RC4
    secure_rc4_context* ctx = init_secure_rc4(key, key_len, salt);
    if (!ctx) {
        fprintf(stderr, "Ошибка при инициализации защищённой области для RC4\n");
        free(buffer);
        dlclose(handle);
        return 1;
    }

    // 5. Шифрование с использованием RC4
    if (rc4_crypt(ctx, buffer, buffer, len) != 0) {
        fprintf(stderr, "Ошибка при шифровании RC4\n");
        clear_secure_rc4(ctx);
        secure_memset(buffer, 0, len);
        free(buffer);
        dlclose(handle);
        return 1;
    }

    // 6. Запись зашифванного файла
    FILE* fout = fopen(out_path, "wb");
    if (!fout) {
        perror("fopen output");
        clear_secure_rc4(ctx);
        secure_memset(buffer, 0, len);
        free(buffer);
        dlclose(handle);
        return 1;
    }
    fwrite(buffer, 1, len, fout);
    fclose(fout);

    // 7. Безопасное удаление ключа и состояния из памяти
    clear_secure_rc4(ctx);

    // Безопасное затирание буфера перед освобождением
    secure_memset(buffer, 0, len);
    free(buffer);

    dlclose(handle);

    printf("Файл успешно зашифрован: %s -> %s\n", in_path, out_path);
    return 0;
}
