#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>

typedef void (*set_key_func)(char);
typedef void (*caesar_func)(void*, void*, int);
typedef int (*init_secure_key_func)(void);
typedef void (*clear_key_func)(void);

int main(int argc, char* argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Использование: %s <путь_к_библиотеке> <ключ> <входной_файл> <выходной_файл>\n", argv);
        return 1;
    }
    
    const char* lib_path = argv;
    char key = (char)atoi(argv);
    const char* in_path = argv;
    const char* out_path = argv;
    
    // 1. Динамическая загрузка
    void* handle = dlopen(lib_path, RTLD_LAZY);
    if (!handle) {
        fprintf(stderr, "Ошибка при загрузке библиотеки: %s\n", dlerror());
        return 1;
    }
    
    // 2. Получение адресов функций
    set_key_func set_key = (set_key_func)dlsym(handle, "set_key");
    caesar_func caesar = (caesar_func)dlsym(handle, "caesar");
    init_secure_key_func init_secure_key = (init_secure_key_func)dlsym(handle, "init_secure_key");
    clear_key_func clear_key = (clear_key_func)dlsym(handle, "clear_key");
    
    if (!set_key || !caesar || !init_secure_key || !clear_key) {
        fprintf(stderr, "Ошибка при поиске символов: %s\n", dlerror());
        dlclose(handle);
        return 1;
    }
    
    // 3. Чтение файла
    FILE* fin = fopen(in_path, "rb");
    if (!fin) { perror("fopen input"); return 1; }
    
    fseek(fin, 0, SEEK_END);
    long len = ftell(fin);
    fseek(fin, 0, SEEK_SET);
    
    void* buffer = malloc(len);
    if (!buffer) { perror("malloc"); return 1; }
    
    fread(buffer, 1, len, fin);
    fclose(fin);
    
    // 4. Инициализация защищённой области памяти для ключа
    if (init_secure_key() < 0) {
        fprintf(stderr, "Ошибка при инициализации защищённой области для ключа\n");
        free(buffer);
        dlclose(handle);
        return 1;
    }
    
    // 5. Шифрование
    set_key(key);
    caesar(buffer, buffer, len); // Шифруем на месте
    
    // 6. Запись файла
    FILE* fout = fopen(out_path, "wb");
    if (!fout) { perror("fopen output"); return 1; }
    fwrite(buffer, 1, len, fout);
    fclose(fout);
    
    // 7. Безопасное удаление ключа из памяти
    clear_key();
    
    free(buffer);
    dlclose(handle);
    
    return 0;
}
