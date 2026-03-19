/**
 * @file test_loader.c
 * @brief Утилита для тестирования libcaesar с динамической загрузкой
 * 
 * Поддерживает:
 * - Единичное шифрование файла
 * - Пакетный режим обработки нескольких файлов (--batch)
 * - Логирование в формате, совместимом с secure_copy
 * - Проверку целостности (шифрование + дешифрование)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Типы функций из libcaesar */
typedef void (*set_key_func)(char);
typedef void (*caesar_func)(void*, void*, int);

/* === Логирование (совместимый формат с secure_copy) === */
static void log_test(const char *operation, const char *file, 
                     const char *result, double elapsed) {
    FILE *log = fopen("log.txt", "a");
    if (!log) return;
    
    time_t now = time(NULL);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
    
    fprintf(log, "[%s] [Thread:TEST] [File:%s] [%s] [Result:%s] [Time:%.3fs]\n",
            ts, file, operation, result, elapsed);
    fclose(log);
}

/* === Вспомогательные функции === */
static double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int ensure_parent_dir(const char *path) {
    char *tmp = strdup(path);
    char *last_slash = strrchr(tmp, '/');
    if (last_slash) {
        *last_slash = '\0';
        mkdir(tmp, 0755);  // Игнорируем ошибки, если директория уже есть
    }
    free(tmp);
    return 0;
}

/* === Основная функция шифрования одного файла === */
static int process_file(void *handle, set_key_func set_key, caesar_func caesar,
                        const char *in_path, const char *out_path, char key) {
    double start = get_time_ms();
    
    /* Чтение */
    FILE *fin = fopen(in_path, "rb");
    if (!fin) {
        log_test("READ", in_path, "ERROR:fopen", 0);
        fprintf(stderr, "Cannot open input: %s\n", in_path);
        return 1;
    }
    fseek(fin, 0, SEEK_END);
    long len = ftell(fin);
    fseek(fin, 0, SEEK_SET);
    
    unsigned char *buffer = malloc(len);
    if (!buffer) {
        fclose(fin);
        log_test("READ", in_path, "ERROR:malloc", 0);
        return 1;
    }
    fread(buffer, 1, len, fin);
    fclose(fin);
    
    /* Шифрование */
    set_key(key);
    caesar(buffer, buffer, (int)len);
    
    /* Запись */
    ensure_parent_dir(out_path);
    FILE *fout = fopen(out_path, "wb");
    if (!fout) {
        free(buffer);
        log_test("WRITE", out_path, "ERROR:fopen", 0);
        fprintf(stderr, "Cannot open output: %s\n", out_path);
        return 1;
    }
    fwrite(buffer, 1, len, fout);
    fclose(fout);
    free(buffer);
    
    double elapsed = get_time_ms() - start;
    log_test("ENCRYPT", in_path, "SUCCESS", elapsed);
    return 0;
}

/* === Проверка: шифрование + дешифрование === */
static int verify_roundtrip(void *handle, set_key_func set_key, caesar_func caesar,
                            const char *test_file, char key) {
    const char *enc_file = "test_enc.tmp";
    const char *dec_file = "test_dec.tmp";
    
    if (process_file(handle, set_key, caesar, test_file, enc_file, key) != 0)
        return 1;
    
    /* Дешифрование тем же ключом (XOR обратим) */
    if (process_file(handle, set_key, caesar, enc_file, dec_file, key) != 0)
        return 1;
    
    /* Сравнение оригинала и результата */
    FILE *orig = fopen(test_file, "rb");
    FILE *dec = fopen(dec_file, "rb");
    if (!orig || !dec) {
        fprintf(stderr, "Verification failed: cannot compare files\n");
        return 1;
    }
    
    int c1, c2, match = 1;
    while ((c1 = fgetc(orig)) != EOF && (c2 = fgetc(dec)) != EOF) {
        if (c1 != c2) { match = 0; break; }
    }
    if (fgetc(orig) != EOF || fgetc(dec) != EOF) match = 0;
    
    fclose(orig); fclose(dec);
    unlink(enc_file); unlink(dec_file);
    
    if (match) {
        fprintf(stdout, "✓ Roundtrip verification PASSED for %s\n", test_file);
        return 0;
    } else {
        fprintf(stderr, "✗ Roundtrip verification FAILED for %s\n", test_file);
        return 1;
    }
}

/* === Печать справки === */
static void print_help(const char *prog) {
    fprintf(stderr, 
        "Usage: %s <lib_path> <key> <input> <output> [options]\n"
        "       %s --batch <lib_path> <key> <out_dir> file1 file2 ...\n"
        "       %s --verify <lib_path> <key> <test_file>\n"
        "\nOptions:\n"
        "  --batch   Process multiple files, save to out_dir with original names\n"
        "  --verify  Encrypt then decrypt, compare with original (integrity check)\n"
        "  --help    Show this message\n"
        "\nExample:\n"
        "  %s ./libcaesar.so 75 input.txt output/encrypted.txt\n"
        "  %s --batch ./libcaesar.so 75 out/ f1.txt f2.txt f3.txt\n",
        prog, prog, prog, prog, prog);
}

/* === main === */
int main(int argc, char *argv[]) {
    /* Обработка опций */
    if (argc < 2) { print_help(argv[0]); return 1; }
    
    int batch_mode = 0, verify_mode = 0;
    int arg_offset = 1;
    
    if (strcmp(argv[1], "--batch") == 0) {
        batch_mode = 1; arg_offset = 2;
    } else if (strcmp(argv[1], "--verify") == 0) {
        verify_mode = 1; arg_offset = 2;
    } else if (strcmp(argv[1], "--help") == 0) {
        print_help(argv[0]); return 0;
    }
    
    /* Загрузка библиотеки (один раз) */
    if (argc < arg_offset + 2) { print_help(argv[0]); return 1; }
    
    const char *lib_path = argv[arg_offset];
    char key = (char)atoi(argv[arg_offset + 1]);
    
    void *handle = dlopen(lib_path, RTLD_LAZY);
    if (!handle) {
        fprintf(stderr, "dlopen error: %s\n", dlerror());
        return 2;
    }
    
    set_key_func set_key = (set_key_func)dlsym(handle, "set_key");
    caesar_func caesar = (caesar_func)dlsym(handle, "caesar");
    if (!set_key || !caesar) {
        fprintf(stderr, "dlsym error: %s\n", dlerror());
        dlclose(handle);
        return 2;
    }
    
    int exit_code = 0;
    
    if (verify_mode) {
        /* Режим проверки целостности */
        if (argc < arg_offset + 3) {
            fprintf(stderr, "--verify requires <test_file>\n");
            exit_code = 1;
        } else {
            exit_code = verify_roundtrip(handle, set_key, caesar, 
                                        argv[arg_offset + 2], key);
        }
    } 
    else if (batch_mode) {
        /* Пакетный режим: test_loader --batch lib key out_dir f1 f2 f3 */
        if (argc < arg_offset + 4) {
            fprintf(stderr, "--batch requires <out_dir> and at least one file\n");
            exit_code = 1;
        } else {
            const char *out_dir = argv[arg_offset + 2];
            mkdir(out_dir, 0755);
            
            for (int i = arg_offset + 3; i < argc; i++) {
                const char *in = argv[i];
                const char *base = strrchr(in, '/');
                base = base ? base + 1 : in;
                
                char out_path[512];
                snprintf(out_path, sizeof(out_path), "%s/%s", out_dir, base);
                
                if (process_file(handle, set_key, caesar, in, out_path, key) != 0) {
                    exit_code = 1;
                    fprintf(stderr, "Failed: %s\n", in);
                } else {
                    fprintf(stdout, "✓ %s → %s\n", in, out_path);
                }
            }
        }
    } 
    else {
        /* Обычный режим: 4 аргумента после опций */
        if (argc != arg_offset + 4) {
            print_help(argv[0]);
            exit_code = 1;
        } else {
            const char *in = argv[arg_offset + 2];
            const char *out = argv[arg_offset + 3];
            exit_code = process_file(handle, set_key, caesar, in, out, key);
            if (exit_code == 0)
                fprintf(stdout, "✓ Encrypted: %s → %s\n", in, out);
        }
    }
    
    dlclose(handle);
    log_test("EXIT", "test_loader", exit_code == 0 ? "SUCCESS" : "ERROR", 0);
    return exit_code;
}
