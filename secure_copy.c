#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include <libgen.h>

#include "libcaesar.h"

/* === КОНФИГУРАЦИЯ === */
#define NUM_WORKERS 3
#define DEADLOCK_TIMEOUT_SEC 5
#define MAX_PATH_LEN 2048

/* === ГЛОБАЛЬНЫЕ РЕСУРСЫ (защищены одним мьютексом) === */
static int copied_count = 0;                    /* Счётчик успешно обработанных файлов */
static char **file_queue = NULL;                /* Очередь файлов для обработки */
static int queue_total = 0;                     /* Общее количество файлов */
static volatile int queue_next = 0;             /* Индекс следующего файла */
static char *output_directory = NULL;           /* Путь к выходной директории */
static pthread_mutex_t sync_mutex = PTHREAD_MUTEX_INITIALIZER; /* ЕДИНСТВЕННЫЙ мьютекс */

/* === Флаг завершения === */
static volatile sig_atomic_t keep_running = 1;

/* === Обработчик сигнала SIGINT === */
static void sigint_handler(int sig) {
    (void)sig;
    keep_running = 0;
    fprintf(stderr, "\n[INFO] Получен сигнал прерывания, завершение работы...\n");
}

/* === Вспомогательная: текущее время в секундах === */
static double get_time_seconds(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

/* === Вспомогательная: рекурсивное создание директории === */
static int ensure_directory(const char *path) {
    if (!path || !*path) return -1;
    
    char tmp[MAX_PATH_LEN];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    
    char *p = tmp;
    if (*p == '/') p++;
    
    while (*p) {
        while (*p && *p != '/') p++;
        char saved = *p;
        *p = '\0';
        
        struct stat st;
        if (stat(tmp, &st) != 0) {
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
        }
        
        *p = saved;
        if (*p) p++;
    }
    return 0;
}

/* === Логирование операции (защищено мьютексом с deadlock detection) === */
static void log_operation(const char *thread_id, const char *filename,
                          const char *status, double exec_time) {
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += DEADLOCK_TIMEOUT_SEC;

    int lock_result = pthread_mutex_timedlock(&sync_mutex, &timeout);
    if (lock_result == ETIMEDOUT) {
        fprintf(stderr, "Возможная взаимоблокировка: поток %s ожидает мьютекс более %d секунд\n",
                thread_id, DEADLOCK_TIMEOUT_SEC);
        /* Fallback: повторный захват без таймаута для продолжения работы */
        pthread_mutex_lock(&sync_mutex);
    }

    FILE *log = fopen("log.txt", "a");
    if (log) {
        time_t now = time(NULL);
        char timestamp[64];
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));

        fprintf(log, "[%s] [Thread:%s] [File:%s] [Status:%s] [Time:%.3fs]\n",
                timestamp, thread_id, filename, status, exec_time);
        fflush(log);  /* Гарантируем запись на диск */
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

/* === Инкремент счётчика (с минимальной критической секцией) === */
static void increment_copied_count(void) {
    pthread_mutex_lock(&sync_mutex);
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
        int write_success = 0;

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

        if (file_size < 0) {
            fclose(in);
            double elapsed = get_time_seconds() - start_time;
            log_operation(thread_id, filename, "ERROR:ftell", elapsed);
            continue;
        }

        unsigned char *buffer = malloc((size_t)file_size + 1);
        if (!buffer) {
            fclose(in);
            double elapsed = get_time_seconds() - start_time;
            log_operation(thread_id, filename, "ERROR:malloc", elapsed);
            continue;
        }

        size_t bytes_read = fread(buffer, 1, (size_t)file_size, in);
        fclose(in);

        /* === Шифрование === */
        unsigned char *encrypted = malloc(bytes_read + 1);
        if (!encrypted) {
            free(buffer);
            double elapsed = get_time_seconds() - start_time;
            log_operation(thread_id, filename, "ERROR:malloc_enc", elapsed);
            continue;
        }

        caesar(buffer, encrypted, (int)bytes_read);
        free(buffer);

        /* === Формирование выходного пути === */
        char *base_name = basename(filename);
        char output_path[MAX_PATH_LEN];
        snprintf(output_path, sizeof(output_path), "%s/%s", output_directory, base_name);

        /* === Запись === */
        FILE *out = fopen(output_path, "wb");
        if (out) {
            size_t written = fwrite(encrypted, 1, bytes_read, out);
            fclose(out);
            if (written != (size_t)bytes_read) {
                status = "ERROR:write_incomplete";
            } else {
                write_success = 1;
            }
        } else {
            status = "ERROR:open_write";
        }
        free(encrypted);

        /* === Логирование и обновление счётчика === */
        double elapsed = get_time_seconds() - start_time;
        log_operation(thread_id, filename, status, elapsed);

        if (write_success) {
            increment_copied_count();
        }
    }

    return NULL;
}

/* === Парсинг output_dir/key === */
static int parse_output_arg(const char *arg, char **out_dir, char *key) {
    if (!arg || !out_dir || !key) return -1;
    
    const char *slash = strrchr(arg, '/');
    if (!slash) return -1;

    size_t dir_len = (size_t)(slash - arg);
    *out_dir = malloc(dir_len + 2);  /* +1 для '\0', +1 на всякий случай */
    if (!*out_dir) return -1;

    strncpy(*out_dir, arg, dir_len);
    (*out_dir)[dir_len] = '\0';

    *key = (*(slash + 1) != '\0') ? *(slash + 1) : 0x42;  /* Ключ по умолчанию */
    return 0;
}

/* === Печать справки === */
static void print_help(const char *prog) {
    fprintf(stderr,
        "Использование: %s file1.txt [file2.txt ...] output_dir/key\n"
        "\nПараметры:\n"
        "  file1.txt ...   Входные файлы для шифрования\n"
        "  output_dir/key  Выходная директория и ключ шифрования (последний байт)\n"
        "\nПримеры:\n"
        "  %s a.txt b.txt c.txt out/7\n"
        "  %s *.log backup/K\n"
        "\nРезультат:\n"
        "  - Зашифрованные файлы в output_dir с оригинальными именами\n"
        "  - Лог операций в log.txt\n"
        "  - Статистика в stdout после завершения\n",
        prog, prog, prog);
}

/* === main === */
int main(int argc, char *argv[]) {
    if (argc < 3) {
        print_help(argv[0]);
        return 1;
    }

    /* Обработчик сигнала прерывания */
    signal(SIGINT, sigint_handler);

    /* Парсинг последнего аргумента: output_dir/key */
    char key = 0;
    if (parse_output_arg(argv[argc - 1], &output_directory, &key) != 0) {
        fprintf(stderr, "Ошибка: последний аргумент должен быть в формате output_dir/key\n");
        print_help(argv[0]);
        return 1;
    }

    /* Установка ключа шифрования (ДО создания потоков!) */
    set_key(key);

    /* Создание выходной директории */
    if (ensure_directory(output_directory) != 0) {
        perror("Ошибка создания выходной директории");
        free(output_directory);
        return 1;
    }

    /* Инициализация очереди файлов (исключаем последний аргумент) */
    queue_total = argc - 2;
    file_queue = argv + 1;

    fprintf(stdout, "[INFO] Запуск: %d файлов, ключ=0x%02X, выход=%s\n", 
            queue_total, (unsigned char)key, output_directory);

    /* Создание 3 рабочих потоков */
    pthread_t workers[NUM_WORKERS];
    int create_error = 0;
    
    for (int i = 0; i < NUM_WORKERS; i++) {
        if (pthread_create(&workers[i], NULL, worker_thread, NULL) != 0) {
            perror("Ошибка создания потока");
            create_error = 1;
            keep_running = 0;
            break;
        }
    }

    /* Ожидание завершения потоков */
    for (int i = 0; i < NUM_WORKERS; i++) {
        if (!create_error) {
            pthread_join(workers[i], NULL);
        }
    }

    /* Итоговая статистика */
    int final_count = get_copied_count();
    printf("\n=== Результат ===\n");
    printf("Обработано файлов: %d из %d\n", final_count, queue_total);
    printf("Лог сохранён в: log.txt\n");

    /* Очистка ресурсов */
    free(output_directory);
    pthread_mutex_destroy(&sync_mutex);

    return (final_count == queue_total) ? 0 : 1;
}
