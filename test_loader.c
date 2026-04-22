#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>

typedef void (*set_key_func)(char);
typedef void (*caesar_func)(void*, void*, int);

int main(int argc, char* argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Использование: %s <путь_к_библиотеке> <ключ> <входной_файл> <выходной_файл>\n", argv[0]);
        return 1;
    }
    
    const char* lib_path = argv[1];
    char key = (char)atoi(argv[2]);
    const char* in_path = argv[3];
    const char* out_path = argv[4];
    
    // 1. Динамическая загрузка
    void* handle = dlopen(lib_path, RTLD_LAZY);
    if (!handle) {
        fprintf(stderr, "Ошибка при загрузке библиотеки: %s\n", dlerror());
        return 1;
    }
    
    // 2. Получение адресов функций
    set_key_func set_key = (set_key_func)dlsym(handle, "set_key");
    caesar_func caesar = (caesar_func)dlsym(handle, "caesar");
    
    if (!set_key || !caesar) {
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
    
    // 4. Шифрование
    set_key(key);
    caesar(buffer, buffer, len); // Шифруем на месте
    
    // 5. Запись файла
    FILE* fout = fopen(out_path, "wb");
    if (!fout) { perror("fopen output"); return 1; }
    fwrite(buffer, 1, len, fout);
    fclose(fout);
    
    free(buffer);
    dlclose(handle);
    
    return 0;
}
