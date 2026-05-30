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

#define BUFFER_SIZE 4096
#define WORKERS_COUNT 4
#define MAX_PATH_LEN 1024
#define SALT_SIZE 16
#define CONTAINER_MAGIC 0xCAFEBABE

typedef struct {
    char input_path[MAX_PATH_LEN];
    char container_path[MAX_PATH_LEN];
    uint8_t key[RC4_KEY_MAX_SIZE];
    size_t key_len;
    uint8_t salt[SALT_SIZE];
} FileTask;

typedef struct {
    FileTask tasks[100];
    int head, tail, count;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty, not_full;
} FileQueue;

typedef struct {
    double total_time;
    double avg_time_per_file;
    int files_processed;
} Statistics;

volatile int keep_running = 1;
FileQueue queue;
Statistics stats_sequential = {0};
Statistics stats_parallel = {0};

secure_rc4_context* (*lib_init_secure_rc4)(const uint8_t*, size_t, const uint8_t*);
int (*lib_rc4_crypt)(secure_rc4_context*, const void*, void*, size_t);
void (*lib_clear_secure_rc4)(secure_rc4_context*);

void sigint_handler(int sig) { (void)sig; keep_running = 0; }

void queue_init(FileQueue* q) {
    q->head = q->tail = q->count = 0;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

void queue_push(FileQueue* q, FileTask task) {
    pthread_mutex_lock(&q->mutex);
    while (q->count == 100 && keep_running)
        pthread_cond_wait(&q->not_full, &q->mutex);
    if (!keep_running) { pthread_mutex_unlock(&q->mutex); return; }
    
    q->tasks[q->tail] = task;
    q->tail = (q->tail + 1) % 100;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
}

int queue_pop(FileQueue* q, FileTask* task) {
    pthread_mutex_lock(&q->mutex);
    while (q->count == 0 && keep_running)
        pthread_cond_wait(&q->not_empty, &q->mutex);
    if (!keep_running && q->count == 0) { pthread_mutex_unlock(&q->mutex); return 0; }
    
    *task = q->tasks[q->head];
    q->head = (q->head + 1) % 100;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    return 1;
}

double get_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int hex_to_bytes(const char* hex, uint8_t* bytes, size_t max_len) {
    size_t len = strlen(hex) / 2;
    if (len > max_len) len = max_len;
    for (size_t i = 0; i < len; i++) sscanf(hex + 2 * i, "%2hhx", &bytes[i]);
    return len;
}

int process_file_to_container(const char* input_path, const char* container_path,
                              const uint8_t* key, size_t key_len, const uint8_t* salt) {
    FILE* fin = fopen(input_path, "rb");
    if (!fin) { perror("fopen input"); return -1; }
    fseek(fin, 0, SEEK_END);
    long file_len = ftell(fin);
    fseek(fin, 0, SEEK_SET);
    
    void* buffer = malloc(file_len);
    if (!buffer) { fclose(fin); return -1; }
    fread(buffer, 1, file_len, fin);
    fclose(fin);

    secure_rc4_context* ctx = lib_init_secure_rc4(key, key_len, salt);
    if (!ctx) { free(buffer); return -1; }

    if (lib_rc4_crypt(ctx, buffer, buffer, file_len) != 0) {
        lib_clear_secure_rc4(ctx); free(buffer); return -1;
    }

    FILE* fout = fopen(container_path, "ab");
    if (!fout) { lib_clear_secure_rc4(ctx); free(buffer); return -1; }

    uint32_t name_len = (uint32_t)strlen(input_path);
    uint32_t f_len = (uint32_t)file_len;
    fwrite(&f_len, sizeof(uint32_t), 1, fout);
    fwrite(&name_len, sizeof(uint32_t), 1, fout);
    fwrite(salt, 1, SALT_SIZE, fout);
    fwrite(input_path, 1, name_len, fout);
    fwrite(buffer, 1, file_len, fout);
    fclose(fout);
    lib_clear_secure_rc4(ctx);
    secure_memset(buffer, 0, file_len);
    free(buffer);
    return 0;
}

void* worker_thread(void* arg) {
    (void)arg;
    FileTask task;
    while (keep_running) {
        if (!queue_pop(&queue, &task)) break;
        process_file_to_container(task.input_path, task.container_path,
                                  task.key, task.key_len, task.salt);
    }
    return NULL;
}

void run_sequential(char** files, int file_count, const uint8_t* key,
                    size_t key_len, const uint8_t* salt, const char* container_path) {
    printf("\n=== ПОСЛЕДОВАТЕЛЬНЫЙ РЕЖИМ ===\n");
    printf("Обработка %d файлов...\n", file_count);
    double start_time = get_time();
    
    for (int i = 0; i < file_count; i++) {
        double file_start = get_time();
        if (process_file_to_container(files[i], container_path, key, key_len, salt) == 0)
            printf("Файл %d: %s -> %s (%.3f сек)\n", i+1, files[i], container_path, get_time()-file_start);
        else printf("Ошибка при обработке файла %s\n", files[i]);
    }
    stats_sequential.total_time = get_time() - start_time;
    stats_sequential.avg_time_per_file = stats_sequential.total_time / file_count;
    stats_sequential.files_processed = file_count;
}

void run_parallel(char** files, int file_count, const uint8_t* key,
                  size_t key_len, const uint8_t* salt, const char* container_path) {
    printf("\n=== ПАРАЛЛЕЛЬНЫЙ РЕЖИМ ===\n");
    printf("Обработка %d файлов в %d потоков...\n", file_count, WORKERS_COUNT);
    queue_init(&queue);
    
    double start_time = get_time(); // Исправлено: добавлена переменная
    pthread_t workers[WORKERS_COUNT];
    for (int i = 0; i < WORKERS_COUNT; i++)
        pthread_create(&workers[i], NULL, worker_thread, NULL);

    for (int i = 0; i < file_count; i++) {
        FileTask task;
        snprintf(task.input_path, sizeof(task.input_path), "%s", files[i]);
        snprintf(task.container_path, sizeof(task.container_path), "%s", container_path);
        memcpy(task.key, key, key_len); task.key_len = key_len;
        memcpy(task.salt, salt, SALT_SIZE);
        queue_push(&queue, task);
    }

    keep_running = 0;
    pthread_cond_broadcast(&queue.not_empty);
    for (int i = 0; i < WORKERS_COUNT; i++) pthread_join(workers[i], NULL);

    stats_parallel.total_time = get_time() - start_time;
    stats_parallel.avg_time_per_file = stats_parallel.total_time / file_count;
    stats_parallel.files_processed = file_count;
}

void print_comparison() {
    printf("\n=== СРАВНИТЕЛЬНАЯ ТАБЛИЦА ===\n");
    printf("| %-15s | %-12s | %-12s |\n", "Режим", "Время (сек)", "Ср. на файл");
    printf("| %-15s | %-12.3f | %-12.3f |\n", "Sequential", stats_sequential.total_time, stats_sequential.avg_time_per_file);
    printf("| %-15s | %-12.3f | %-12.3f |\n", "Parallel", stats_parallel.total_time, stats_parallel.avg_time_per_file);
}

void* load_library(const char* lib_path) {
    void* handle = dlopen(lib_path, RTLD_LAZY);
    if (!handle) { fprintf(stderr, "dlopen: %s\n", dlerror()); return NULL; }
    lib_init_secure_rc4 = dlsym(handle, "init_secure_rc4");
    lib_rc4_crypt = dlsym(handle, "rc4_crypt");
    lib_clear_secure_rc4 = dlsym(handle, "clear_secure_rc4");
    if (!lib_init_secure_rc4 || !lib_rc4_crypt || !lib_clear_secure_rc4) {
        fprintf(stderr, "dlsym: %s\n", dlerror()); dlclose(handle); return NULL;
    }
    return handle;
}

int main(int argc, char* argv[]) {
    signal(SIGINT, sigint_handler);
    if (argc < 6) {
        fprintf(stderr, "Использование: %s <lib.so> <key_hex> <salt_hex> <container> <mode> <file1> ...\n", argv[0]);
        return 1;
    }

    const char *lib_path = argv[1], *key_hex = argv[2], *salt_hex = argv[3];
    const char *container_path = argv[4], *mode_str = argv[5];
    
    uint8_t key[RC4_KEY_MAX_SIZE], salt[SALT_SIZE];
    size_t key_len = hex_to_bytes(key_hex, key, RC4_KEY_MAX_SIZE);
    hex_to_bytes(salt_hex, salt, SALT_SIZE);

    void* handle = load_library(lib_path);
    if (!handle) return 1;

    enum { SEQ, PAR, AUTO } mode;
    if (strcmp(mode_str, "--mode=sequential") == 0) mode = SEQ;
    else if (strcmp(mode_str, "--mode=parallel") == 0) mode = PAR;
    else if (strcmp(mode_str, "--mode=auto") == 0) mode = AUTO;
    else { fprintf(stderr, "Неверный режим\n"); dlclose(handle); return 1; }

    int file_count = argc - 6;
    char** files = &argv[6];

    FILE* container = fopen(container_path, "wb");
    if (!container) { fprintf(stderr, "Не удалось создать контейнер\n"); dlclose(handle); return 1; }
    uint32_t magic = CONTAINER_MAGIC;
    fwrite(&magic, sizeof(uint32_t), 1, container);
    fclose(container);

    double start_total = get_time();
    if (mode == SEQ) run_sequential(files, file_count, key, key_len, salt, container_path);
    else if (mode == PAR) run_parallel(files, file_count, key, key_len, salt, container_path);
    else {
        if (file_count > 10) run_parallel(files, file_count, key, key_len, salt, container_path);
        else run_sequential(files, file_count, key, key_len, salt, container_path);
    }

    printf("\nОбщее время: %.3f сек\n", get_time() - start_total);
    print_comparison();
    dlclose(handle);
    return 0;
}
