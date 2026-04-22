#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
#include "libcaesar.h"

// Параметры
#define BUFFER_SIZE 4096
#define WORKERS_COUNT 4

// Структура для файла в очереди
typedef struct {
    char input_path[256];
    char output_path[256];
    char key;
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
void (*lib_set_key)(char);
void (*lib_caesar)(void*, void*, int);

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

// Обработка одного файла
void process_file(const char* input_path, const char* output_path, char key) {
    // Чтение файла
    FILE* fin = fopen(input_path, "rb");
    if (!fin) {
        perror("fopen input");
        return;
    }
    
    fseek(fin, 0, SEEK_END);
    long len = ftell(fin);
    fseek(fin, 0, SEEK_SET);
    
    void* buffer = malloc(len);
    if (!buffer) {
        perror("malloc");
        fclose(fin);
        return;
    }
    
    fread(buffer, 1, len, fin);
    fclose(fin);
    
    // Шифрование
    lib_set_key(key);
    lib_caesar(buffer, buffer, len);
    
    // Запись файла
    FILE* fout = fopen(output_path, "wb");
    if (!fout) {
        perror("fopen output");
        free(buffer);
        return;
    }
    
    fwrite(buffer, 1, len, fout);
    fclose(fout);
    free(buffer);
}

// Функция потока-работника для параллельного режима
typedef struct {
    int id;
} WorkerArg;

void* worker_thread(void* arg) {
    //WorkerArg* warg = (WorkerArg*)arg;
    (void)arg;
    FileTask task;
    
    while (keep_running) {
        if (!queue_pop(&queue, &task)) {
            break;
        }
        
        process_file(task.input_path, task.output_path, task.key);
    }
    
    return NULL;
}

// Последовательный режим
void run_sequential(char** files, int file_count, char key) {
    printf("\n=== ПОСЛЕДОВАТЕЛЬНЫЙ РЕЖИМ ===\n");
    printf("Обработка %d файлов...\n", file_count);
    
    double start_time = get_time();
    
    for (int i = 0; i < file_count; i++) {
        char output_path[256];
        snprintf(output_path, sizeof(output_path), "encrypted_%s", files[i]);
        
        double file_start = get_time();
        process_file(files[i], output_path, key);
        double file_end = get_time();
        
        printf("Файл %d: %s -> %s (%.3f сек)\n", 
               i + 1, files[i], output_path, file_end - file_start);
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
void run_parallel(char** files, int file_count, char key) {
    printf("\n=== ПАРАЛЛЕЛЬНЫЙ РЕЖИМ ===\n");
    printf("Обработка %d файлов в %d потоков...\n", file_count, WORKERS_COUNT);
    
    queue_init(&queue);
    
    // Создание потоков
    pthread_t workers[WORKERS_COUNT];
    WorkerArg args[WORKERS_COUNT];
    
    for (int i = 0; i < WORKERS_COUNT; i++) {
        args[i].id = i;
        pthread_create(&workers[i], NULL, worker_thread, &args[i]);
    }
    
    // Добавление задач в очередь
    for (int i = 0; i < file_count; i++) {
        FileTask task;
        snprintf(task.input_path, sizeof(task.input_path), "%s", files[i]);
        snprintf(task.output_path, sizeof(task.output_path), "encrypted_%s", files[i]);
        task.key = key;
        
        queue_push(&queue, task);
    }
    
    // Ожидание завершения всех задач
    while (queue.count > 0) {
        usleep(10000); // 10 мс
    }
    
    // Остановка потоков
    keep_running = 0;
    pthread_cond_broadcast(&queue.not_empty);
    
    // Ожидание завершения потоков
    for (int i = 0; i < WORKERS_COUNT; i++) {
        pthread_join(workers[i], NULL);
    }
    
    printf("\n--- Статистика (параллельный режим) ---\n");
    printf("Всего файлов: %d\n", file_count);
    printf("Потоков: %d\n", WORKERS_COUNT);
    printf("Обработано файлов: %d\n", file_count);
}

// Вывод сравнительной таблицы
void print_comparison() {
    printf("\n=== СРАВНИТЕЛЬНАЯ ТАБЛИЦА РЕЖИМОВ ===\n");
    printf("--------------------------------------------------\n");
    printf("| %-15s | %-12s | %-12s |\n", "Режим", "Время (сек)", "Ср. на файл");
    printf("--------------------------------------------------\n");
    printf("| %-15s | %-12.3f | %-12.3f |\n", 
           "Sequential", stats_sequential.total_time, stats_sequential.avg_time_per_file);
    printf("--------------------------------------------------\n");
}

// Загрузка библиотеки
void* load_library(const char* lib_path) {
    void* handle = dlopen(lib_path, RTLD_LAZY);
    if (!handle) {
        fprintf(stderr, "Ошибка при загрузке библиотеки: %s\n", dlerror());
        return NULL;
    }
    
    lib_set_key = (void (*)(char))dlsym(handle, "set_key");
    lib_caesar = (void (*)(void*, void*, int))dlsym(handle, "caesar");
    
    if (!lib_set_key || !lib_caesar) {
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
    if (argc < 5) {
        fprintf(stderr, "Использование:\n");
        fprintf(stderr, "  %s <lib_path> <key> <mode> <file1> [file2] ...\n", argv[0]);
        fprintf(stderr, "  mode: --mode=sequential | --mode=parallel | --mode=auto\n");
        return 1;
    }
    
    const char* lib_path = argv[1];
    char key = (char)atoi(argv[2]);
    const char* mode_str = argv[3];
    
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
        return 1;
    }
    
    // Получение списка файлов
    int file_count = argc - 4;
    char** files = &argv[4];
    
    printf("\n=== ШИФРОВАНИЕ ФАЙЛОВ ===\n");
    printf("Библиотека: %s\n", lib_path);
    printf("Ключ: %d\n", key);
    printf("Файлов: %d\n", file_count);
    
    // Автоматический выбор режима
    if (mode == AUTO) {
        if (file_count < 5) {
            mode = SEQUENTIAL;
            printf("Автоматический выбор: последовательный режим (< 5 файлов)\n");
        } else {
            mode = PARALLEL;
            printf("Автоматический выбор: параллельный режим (>= 5 файлов)\n");
        }
    }
    
    // Запуск выбранного режима
    if (mode == SEQUENTIAL) {
        run_sequential(files, file_count, key);
    } else {
        run_parallel(files, file_count, key);
    }
    
    // Вывод сравнительной статистики (если нужно)
    // print_comparison();
    
    dlclose(handle);
    
    printf("\nГотово!\n");
    return 0;
}
