/**
 * @file thread_file.c
 * @brief Многопоточное копирование с шифрованием и синхронизацией
 * 
 * Требования задания:
 * - 3 рабочих потока обрабатывают произвольное количество файлов
 * - Один мьютекс для защиты счётчика и лог-файла
 * - pthread_mutex_timedlock с таймаутом 5 секунд для обнаружения deadlock
 * - Логирование в log.txt в режиме append
 */

#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <errno.h>
#include <time.h>

#include "libcaesar.h"

/* === КОНФИГУРАЦИЯ === */
#define NUM_WORKERS 3
#define DEADLOCK_TIMEOUT_SEC 5

/* === ГЛОБАЛЬНЫЕ РЕСУРСЫ (защищены одним мьютексом) === */
static int copied_count = 0;                    /* Счётчик успешно обработанных файлов */
static char **file_queue = NULL;                /* Очередь файлов для обработки */
static int queue_total = 0;                     /* Общее количество файлов */
static volatile int queue_next = 0;             /* Индекс следующего файла */
static char *output_directory = NULL;           /* Путь к выходной директории */
static pthread_mutex_t sync_mutex = PTHREAD_MUTEX_INITIALIZER;  /* ЕДИНСТВЕННЫЙ мьютекс */

/* === Флаг завершения (для обработки SIGINT) === */
static volatile sig_atomic_t keep_running = 1;

/* === Обработчик сигнала SIGINT === */
void sigint_handler(int sig) {
    (void)sig;
    keep_running = 0;
    fprintf(stderr, "\n[INFO] Получен сигнал прерывания, завершение работы...\n");
}

/* === Вспомогательная: текущее время в секундах === */
static double get_time_seconds(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

/* === Вспомогательная: создание директории === */
static int ensure_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    return mkdir(path, 0755);
}

/* === Логирование операции (защищено мьютексом) === */
static void log_operation(const char *thread_id, const char *filename, 
                          const char *status, double exec_time) {
    /* Захват мьютекса с таймаутом для обнаружения взаимоблокировки */
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += DEADLOCK_TIMEOUT_SEC;
    
    int lock_result = pthread_mutex_timedlock(&sync_mutex, &timeout);
    if (lock_result == ETIMEDOUT) {
        fprintf(stderr, "Возможная взаимоблокировка: поток %s ожидает мьютекс более %d секунд\n",
                thread_id, DEADLOCK_TIMEOUT_SEC);
        /* Fallback: повторный захват без таймаута */
        pthread_mutex_lock(&sync_mutex);
    }
    
    /* Запись в лог-файл */
    FILE *log = fopen("log.txt", "a");
    if (log) {
        time_t now = time(NULL);
        char timestamp[64];
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
        
        fprintf(log, "[%s] [Thread:%s] [File:%s] [Status:%s] [Time:%.3fs]\n",
                timestamp, thread_id, filename, status, exec_time);
        fclose(log);
    }
    
    pthread_mutex_unlock(&sync_mutex);
}

/* === Получение следующего файла из очереди (потокобезопасно) === */
static char* get_next_file(void) {
    pthread_mutex_lock(&sync_mutex);
    char *file = NULL;
    if (queue_next < queue_total && keep_running) {
        file = file_queue[queue_next++];
    }
    pthread_mutex_unlock(&sync_mutex);
    return file;
}

/* === Инкремент счётчика (с защитой от deadlock) === */
static void increment_copied_count(void) {
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += DEADLOCK_TIMEOUT_SEC;
    
    int lock_result = pthread_mutex_timedlock(&sync_mutex, &timeout);
    if (lock_result == ETIMEDOUT) {
        fprintf(stderr, "Возможная взаимоблокировка: поток %lu ожидает мьютекс более %d секунд (increment)\n",
                (unsigned long)pthread_self(), DEADLOCK_TIMEOUT_SEC);
        pthread_mutex_lock(&sync_mutex);
    }
    
    copied_count++;
    pthread_mutex_unlock(&sync_mutex);
}

/* === Получение значения счётчика === */
static int get_copied_count(void) {
    pthread_mutex_lock(&sync_mutex);
    int count = copied_count;
    pthread_mutex_unlock(&sync_mutex);
    return count;
}

/* === Функция рабочего потока === */
static void* worker_thread(void *arg) {
    (void)arg;
    
    /* Уникальный ID потока для логирования */
    char thread_id[32];
    snprintf(thread_id, sizeof(thread_id), "%lu", (unsigned long)pthread_self());
    
    char *filename;
    while (keep_running && (filename = get_next_file()) != NULL) {
        double start_time = get_time_seconds();
        const char *status = "SUCCESS";
        
        /* === Чтение файла === */
        FILE *in = fopen(filename, "rb");
        if (!in) {
            double elapsed = get_time_seconds() - start_time;
            log_operation(thread_id, filename, "ERROR:open_read", elapsed);
            continue;
        }
        
        fseek(in, 0, SEEK_END);
        long file_size = ftell(in);
        fseek(in, 0, SEEK_SET);
        
        unsigned char *buffer = malloc(file_size);
        if (!buffer) {
            fclose(in);
            double elapsed = get_time_seconds() - start_time;
            log_operation(thread_id, filename, "ERROR:malloc", elapsed);
            continue;
        }
        
        size_t bytes_read = fread(buffer, 1, file_size, in);
        fclose(in);
        
        /* === Шифрование === */
        unsigned char *encrypted = malloc(bytes_read);
        if (!encrypted) {
            free(buffer);
            double elapsed = get_time_seconds() - start_time;
            log_operation(thread_id, filename, "ERROR:malloc_enc", elapsed);
            continue;
        }
        
        caesar(buffer, encrypted, (int)bytes_read);
        free(buffer);
        
        /* === Формирование выходного пути === */
        const char *base_name = strrchr(filename, '/');
        base_name = base_name ? base_name + 1 : filename;
        
        char output_path[1024];
        snprintf(output_path, sizeof(output_path), "%s/%s", output_directory, base_name);
        
        /* === Запись === */
        FILE *out = fopen(output_path, "wb");
        if (out) {
            fwrite(encrypted, 1, bytes_read, out);
            fclose(out);
        } else {
            status = "ERROR:open_write";
        }
        free(encrypted);
        
        /* === Логирование и обновление счётчика === */
        double elapsed = get_time_seconds() - start_time;
        log_operation(thread_id, filename, status, elapsed);
        
        if (out) {
            increment_copied_count();
        }
    }
    
    return NULL;
}

/* === Парсинг output_dir/key === */
static int parse_output_arg(const char *arg, char **out_dir, char *key) {
    const char *slash = strrchr(arg, '/');
    if (!slash) return -1;
    
    size_t dir_len = slash - arg;
    *out_dir = malloc(dir_len + 1);
    if (!*out_dir) return -1;
    
    strncpy(*out_dir, arg, dir_len);
    (*out_dir)[dir_len] = '\0';
    
    *key = (*(slash + 1) != '\0') ? *(slash + 1) : 0x42;
    return 0;
}

/* === main === */
int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Использование: %s file1.txt [file2.txt ...] output_dir/key\n", argv[0]);
        return 1;
    }
    
    /* Обработчик SIGINT */
    signal(SIGINT, sigint_handler);
    
    /* Парсинг последнего аргумента */
    char key = 0;
    if (parse_output_arg(argv[argc - 1], &output_directory, &key) != 0) {
        fprintf(stderr, "Ошибка: последний аргумент должен быть в формате output_dir/key\n");
        return 1;
    }
    
    /* Установка ключа (до создания потоков!) */
    set_key(key);
    
    /* Создание выходной директории */
    if (ensure_directory(output_directory) != 0) {
        perror("Ошибка создания выходной директории");
        free(output_directory);
        return 1;
    }
    
    /* Инициализация очереди файлов */
    queue_total = argc - 2;
    file_queue = argv + 1;
    
    /* Создание 3 рабочих потоков */
    pthread_t workers[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; i++) {
        if (pthread_create(&workers[i], NULL, worker_thread, NULL) != 0) {
            perror("Ошибка создания потока");
            keep_running = 0;
            break;
        }
    }
    
    /* Ожидание завершения */
    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_join(workers[i], NULL);
    }
    
    /* Итоговая статистика */
    printf("Обработано файлов: %d из %d\n", get_copied_count(), queue_total);
    
    /* Очистка */
    free(output_directory);
    pthread_mutex_destroy(&sync_mutex);
    
    return 0;
}
