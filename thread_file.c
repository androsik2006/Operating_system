#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "libcaesar.h"

// Константы
#define BUFFER_SIZE 4096
#define WORKERS_COUNT 4  // <= 5 потоков, как требуется
#define MAX_PATH_LEN 1024
#define SALT_SIZE 16
#define CONTAINER_MAGIC 0xCAFEBABE

// Структура для файла в очереди
typedef struct {
    char input_path[MAX_PATH_LEN];
    char container_path[MAX_PATH_LEN];  // Путь к контейнеру
    uint8_t key[RC4_KEY_MAX_SIZE];
    size_t key_len;
    uint8_t salt[SALT_SIZE];
} FileTask;

// Структура очереди файлов
typedef struct {
    FileTask tasks[100];
    int head;
    int tail;
    int count;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
} FileQueue;

// Структура статистики
typedef struct {
    double total_time;
    double avg_time_per_file;
    int files_processed;
} Statistics;

// Глобальные переменные
volatile int keep_running = 1;
FileQueue queue;
Statistics stats_sequential;
Statistics stats_parallel;

// Функции библиотеки (будут загружены динамически)
secure_rc4_context* (*lib_init_secure_rc4)(const uint8_t*, size_t, const uint8_t*);
int (*lib_rc4_crypt)(secure_rc4_context*, const void*, void*, size_t);
void (*lib_clear_secure_rc4)(secure_rc4_context*);

// Обработчик сигнала SIGINT
void sigint_handler(int sig) {
    (void)sig;
    keep_running = 0;
}

// Инициализация очереди
void queue_init(FileQueue* q) {
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
}

// Добавление задачи в очередь
void queue_push(FileQueue* q, FileTask task) {
    pthread_mutex_lock(&q->mutex);
    q->tasks[q->tail] = task;
    q->tail = (q->tail + 1) % 100;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
}

// Получение задачи из очереди
int queue_pop(FileQueue* q, FileTask* task) {
    pthread_mutex_lock(&q->mutex);

    while (q->count == 0 && keep_running) {
        pthread_cond_wait(&q->not_empty, &q->mutex);
    }

    if (!keep_running && q->count == 0) {
        pthread_mutex_unlock(&q->mutex);
        return 0;
    }

    *task = q->tasks[q->head];
    q->head = (q->head + 1) % 100;
    q->count--;

    pthread_mutex_unlock(&q->mutex);
    return 1;
}

// Получение текущего времени в секундах
double get_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// Безопасное затирание памяти
void secure_memset(void* ptr, int value, size_t size) {
    volatile uint8_t* p = (volatile uint8_t*)ptr;
    while (size--) {
        *p++ = (uint8_t)value;
    }
}

// Обработка одного файла и добавление в контейнер
int process_file_to_container(const char* input_path, const char* container_path,
                     const uint8_t* key, size_t key_len, const uint8_t* salt) {
    FILE* fin = fopen(input_path, "rb");
    if (!fin) {
        perror("fopen input");
        return -1;
    }

    fseek(fin, 0, SEEK_END);
    long file_len = ftell(fin);
    fseek(fin, 0, SEEK_SET);

    void* buffer = malloc(file_len);
    if (!buffer) {
        fclose(fin);
        return -1;
    }
    fread(buffer, 1, file_len, fin);
    fclose(fin);

    // Инициализация RC4 контекста
    secure_rc4_context* ctx = lib_init_secure_rc4(key, key_len, salt);
    if (!ctx) {
        free(buffer);
        return -1;
    }

    // Шифрование
    if (lib_rc4_crypt(ctx, buffer, buffer, file_len) != 0) {
        lib_clear_secure_rc4(ctx);
        free(buffer);
        return -1;
    }

    // Запись в контейнер с заголовком
    FILE* fout = fopen(container_path, "ab");  // Добавляем в конец контейнера
    if (!fout) {
        lib_clear_secure_rc4(ctx);
        free(buffer);
        return -1;
    }

    // Заголовок: 4 (длина файла) + 4 (длина имени) + 16 (соль) + N (имя)
    uint32_t name_len = strlen(input_path);
    fwrite(&file_len, sizeof(uint32_t), 1, fout);
    fwrite(&name_len, sizeof(uint32_t), 1, fout);
    fwrite(salt, sizeof(uint8_t), SALT_SIZE, fout);
    fwrite(input_path, sizeof(char), name_len, fout);
    fwrite(buffer, sizeof(uint8_t), file_len, fout);

    fclose(fout);
    lib_clear_secure_rc4(ctx);

    // Безопасное затирание буфера перед освобождением
    secure_memset(buffer, 0, file_len);
    free(buffer);
    return 0;
}

// Функция потока-работника для параллельного режима
void* worker_thread(void* arg) {
    (void)arg;
    FileTask task;

    while (keep_running) {
        if (!queue_pop(&queue, &task)) {
            break;
        }
        process_file_to_container(task.input_path, task.container_path,
                              task.key, task.key_len, task.salt);
    }
    return NULL;
}

// Последовательный режим
void run_sequential(char** files, int file_count, const uint8_t* key,
                   size_t key_len, const uint8_t* salt, const char* container_path) {
    printf("\n=== ПОСЛЕДОВАТЕЛЬНЫЙ РЕЖИМ ===\n");
    printf("Обработка %d файлов...\n", file_count);

    double start_time = get_time();

    for (int i = 0; i < file_count; i++) {
        double file_start = get_time();
        int result = process_file_to_container(files[i], container_path, key, key_len, salt);
        double file_end = get_time();

        if (result == 0) {
            printf("Файл %d: %s -> %s (%.3f сек)\n",
                   i + 1, files[i], container_path, file_end - file_start);
        } else {
            printf("Ошибка при обработке файла %s\n", files[i]);
        }
    }

    double end_time = get_time();

    stats_sequential.total_time = end_time - start_time;
    stats_sequential.avg_time_per_file = stats_sequential.total_time / file_count;
    stats_sequential.files_processed = file_count;

    printf("\n--- Статистика (последовательный режим) ---\n");
    printf("Всего файлов: %d\n", file_count);
    printf("Общее время: %.3f сек\n", stats_sequential.total_time);
    printf("Среднее время на файл: %.3f сек\n", stats_sequential.avg_time_per_file);
}

// Параллельный режим с пулом потоков
void run_parallel(char** files, int file_count, const uint8_t* key,
                  size_t key_len, const uint8_t* salt, const char* container_path) {
    printf("\n=== ПАРАЛЛЕЛЬНЫЙ РЕЖИМ ===\n");
    printf("Обработка %d файлов в %d потоков...\n", file_count, WORKERS_COUNT);

    queue_init(&queue);

    // Создание потоков
    pthread_t workers[WORKERS_COUNT];

    for (int i = 0; i < WORKERS_COUNT; i++) {
        pthread_create(&workers[i], NULL, worker_thread, NULL);
    }

    // Добавление задач в очередь
    for (int i = 0; i < file_count; i++) {
        FileTask task;
        snprintf(task.input_path, sizeof(task.input_path), "%s", files[i]);
        snprintf(task.container_path, sizeof(task.container_path), "%s", container_path);
        memcpy(task.key, key, key_len);
        task.key_len = key_len;
        memcpy(task.salt, salt, SALT_SIZE);

        queue_push(&queue, task);
    }

    // Ожидание завершения всех задач
    while (keep_running && queue.count > 0) {
        usleep(1000); // 1 мс
    }

    // Остановка потоков
    keep_running = 0;
    pthread_cond_broadcast(&queue.not_empty);

    // Ожидание завершения потоков
    for (int i = 0; i < WORKERS_COUNT; i++) {
        pthread_join(workers[i], NULL);
    }

    double end_time = get_time();

    stats_parallel.total_time = end_time - start_time;
    stats_parallel.avg_time_per_file = stats_parallel.total_time / file_count;
    stats_parallel.files_processed = file_count;

    printf("\n--- Статистика (параллельный режим) ---\n");
    printf("Всего файлов: %d\n", file_count);
    printf("Потоков: %d\n", WORKERS_COUNT);
    printf("Общее время: %.3f сек\n", stats_parallel.total_time);
    printf("Среднее время на файл: %.3f сек\n", stats_parallel.avg_time_per_file);
}

// Вывод сравнительной таблицы
void print_comparison() {
    printf("\n=== СРАВНИТЕЛЬНАЯ ТАБЛИЦА РЕЖИМОВ ===\n");
    printf("--------------------------------------------------\n");
    printf("| %-15s | %-12s | %-12s |\n", "Режим", "Время (сек)", "Ср. на файл");
    printf("--------------------------------------------------\n");
    printf("| %-15s | %-12.3f | %-12.3f |\n",
           "Sequential", stats_sequential.total_time, stats_sequential.avg_time_per_file);
    printf("| %-15s | %-12.3f | %-12.3f |\n",
           "Parallel", stats_parallel.total_time, stats_parallel.avg_time_per_file);
    printf("--------------------------------------------------\n");
}

// Загрузка библиотеки
void* load_library(const char* lib_path) {
    void* handle = dlopen(lib_path, RTLD_LAZY);
    if (!handle) {
        fprintf(stderr, "Ошибка при загрузке библиотеки: %s\n", dlerror());
        return NULL;
    }

    lib_init_secure_rc4 = (secure_rc4_context* (*)(const uint8_t*, size_t, const uint8_t*))dlsym(handle, "init_secure_rc4");
    lib_rc4_crypt = (int (*)(secure_rc4_context*, const void*, void*, size_t))dlsym(handle, "rc4_crypt");
    lib_clear_secure_rc4 = (void (*)(secure_rc4_context*))dlsym(handle, "clear_secure_rc4");

    if (!lib_init_secure_rc4 || !lib_rc4_crypt || !lib_clear_secure_rc4) {
        fprintf(stderr, "Ошибка при поиске символов: %s\n", dlerror());
        dlclose(handle);
        return NULL;
    }

    return handle;
}

int main(int argc, char* argv[]) {
    // Регистрация обработчика сигнала
    signal(SIGINT, sigint_handler);

    // Проверка аргументов
    if (argc < 6) {
        fprintf(stderr, "Использование:\n");
        fprintf(stderr, "  %s <lib_path> <key_hex> <salt_hex> <container_path> <mode> <file1> [file2] ...\n", argv[0]);
        fprintf(stderr, "  mode: --mode=sequential | --mode=parallel | --mode=auto\n");
        fprintf(stderr, "  key_hex и salt_hex — в шестнадцатеричном формате\n");
        return 1;
    }

    const char* lib_path = argv[1];
    const char* key_hex = argv[2];
    const char* salt_hex = argv[3];
    const char* container_path = argv[4];
    const char* mode_str = argv[5];

    // Преобразование ключа и соли из hex
    uint8_t key[RC4_KEY_MAX_SIZE];
    size_t key_len = 0;
    uint8_t salt[SALT_SIZE];

    // Простая функция для преобразования hex-строки в байты
    int hex_to_bytes(const char* hex, uint8_t* bytes, size_t max_len) {
        size_t len = strlen(hex) / 2;
        if (len > max_len) len = max_len;

        for (size_t i = 0; i < len; i++) {
            sscanf(hex + 2 * i, "%2hhx", &bytes[i]);
        }
        return len;
    }

    key_len = hex_to_bytes(key_hex, key, RC4_KEY_MAX_SIZE);
    hex_to_bytes(salt_hex, salt, SALT_SIZE);

    // Загрузка библиотеки
    void* handle = load_library(lib_path);
    if (!handle) {
        return 1;
    }

    // Определение режима
    enum { SEQUENTIAL, PARALLEL, AUTO } mode;
    if (strcmp(mode_str, "--mode=sequential") == 0) {
        mode = SEQUENTIAL;
    } else if (strcmp(mode_str, "--mode=parallel") == 0) {
        mode = PARALLEL;
    } else if (strcmp(mode_str, "--mode=auto") == 0) {
        mode = AUTO;
    } else {
        fprintf(stderr, "Неверный режим: %s\n", mode_str);
        dlclose(handle);
        return 1;
    }

    // Получение списка файлов
    int file_count = argc - 6;
    char** files = &argv[6];

    printf("\n=== ШИФРОВАНИЕ ФАЙЛОВ В КОНТЕЙНЕР ===\n");
    printf("Библиотека: %s\n", lib_path);
    printf("Ключ (hex): %s\n", key_hex);
    printf("Соль (hex): %s\n", salt_hex);
    printf("Контейнер: %s\n", container_path);
    printf("Режим: %s\n", mode_str);
    printf("Файлы для обработки: %d\n", file_count);

    // Очистка контейнера перед записью (если режим не append)
    FILE* container = fopen(container_path, "wb");
    if (!container) {
        fprintf(stderr, "Не удалось создать контейнер: %s\n", container_path);
        dlclose(handle);
        return 1;
    }
    // Запись магического числа в начало контейнера
    uint32_t magic = CONTAINER_MAGIC;
    fwrite(&magic, sizeof(uint32_t), 1, container);
    fclose(container);

    double start_time = get_time();

    switch (mode) {
        case SEQUENTIAL:
            run_sequential(files, file_count, key, key_len, salt, container_path);
            break;
        case PARALLEL:
            run_parallel(files, file_count, key, key_len, salt, container_path);
            break;
        case AUTO:
            // Автовыбор режима: если файлов > 10, используем параллельный
            if (file_count > 10) {
                printf("Автовыбор: параллельный режим (файлов > 10)\n");
                run_parallel(files, file_count, key, key_len, salt, container_path);
            } else {
                printf("Автовыбор: последовательный режим (файлов <= 10)\n");
                run_sequential(files, file_count, key, key_len, salt, container_path);
            }
            break;
    }

    double total_end_time = get_time();
    printf("\n=== ОБЩАЯ СТАТИСТИКА ===\n");
    printf("Общее время работы программы: %.3f сек\n", total_end_time - start_time);

    print_comparison();

    // Финальная очистка
    dlclose(handle);

    printf("\nОбработка завершена. Контейнер сохранён: %s\n", container_path);
    return 0;
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
