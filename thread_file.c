/*
 * secure_copy — программа для создания файлового образа диска
 * с сохранением зашифрованных файлов (RC4).
 *
 * Формат записи в образе:
 *   [4 байта] Длина файла (little-endian)
 *   [4 байта] Длина имени файла (little-endian)
 *   [16 байт] Соль
 *   [n байт]  Имя файла
 *   [m байт]  Зашифрованное содержимое файла
 *
 * Использование:
 *   ./secure_copy -add -key <ключ> -image <образ> <файл/директория> ...
 *   ./secure_copy -list -image <образ>
 *   ./secure_copy -get -image <образ> -key <ключ> -out <файл> <имя_в_образе>
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include "libcaesar.h"

/* === Константы === */

#define BUFFER_SIZE     (1024 * 1024)
#define MAX_THREADS     5
#define MAX_PATH_LEN    4096
#define MAX_DEPTH       20
#define INIT_CAPACITY   64

/* === Структуры данных === */

/* Запись из образа (для чтения метаинформации) */
typedef struct {
    uint32_t file_size;
    char    *name;
    uint8_t  salt[SALT_SIZE];
    off_t    data_offset;   /* смещение содержимого файла в образе */
} ImageRecord;

/* Задача для потока при добавлении файла */
typedef struct {
    char         input_path[MAX_PATH_LEN];  /* путь к файлу на диске */
    char         image_name[MAX_PATH_LEN];  /* имя файла внутри образа */
    uint8_t      salt[SALT_SIZE];           /* соль для шифрования */
    off_t        write_offset;              /* предвычисленная позиция записи */
    uint32_t     file_size;                 /* размер файла */
    const char  *image_path;                /* путь к файлу образа */
    const uint8_t *master_key;              /* мастер-ключ */
    size_t       master_key_len;            /* длина мастер-ключа */
    int          success;                   /* флаг успешного выполнения */
} AddTask;

/* === Вспомогательные функции: little-endian I/O === */

static void write_u32_le(uint8_t *buf, uint32_t val) {
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
    buf[2] = (uint8_t)((val >> 16) & 0xFF);
    buf[3] = (uint8_t)((val >> 24) & 0xFF);
}

static uint32_t read_u32_le(const uint8_t *buf) {
    return (uint32_t)buf[0]
         | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
}

/* === Генерация случайной соли === */

static int generate_salt(uint8_t *salt) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) {
        fprintf(stderr, "Ошибка: не удалось открыть /dev/urandom: %s\n", strerror(errno));
        return -1;
    }
    size_t n = fread(salt, 1, SALT_SIZE, f);
    fclose(f);
    if (n != SALT_SIZE) {
        fprintf(stderr, "Ошибка: не удалось прочитать соль из /dev/urandom\n");
        return -1;
    }
    return 0;
}

/* === Сравнение записей по имени (для qsort) === */

static int compare_records_by_name(const void *a, const void *b) {
    const ImageRecord *ra = (const ImageRecord *)a;
    const ImageRecord *rb = (const ImageRecord *)b;
    return strcmp(ra->name, rb->name);
}

/* === Чтение всех записей из образа === */

static int read_image_records(const char *image_path,
                               ImageRecord **out_records, int *out_count) {
    *out_records = NULL;
    *out_count   = 0;

    FILE *f = fopen(image_path, "rb");
    if (!f) {
        /* Образ не существует — это допустимо для -add */
        return 0;
    }

    int capacity = INIT_CAPACITY;
    ImageRecord *records = malloc((size_t)capacity * sizeof(ImageRecord));
    if (!records) {
        fclose(f);
        fprintf(stderr, "Ошибка: не удалось выделить память для записей образа\n");
        return -1;
    }
    int count = 0;

    while (1) {
        uint8_t header[8];
        if (fread(header, 1, 8, f) != 8) break;   /* EOF или ошибка */

        uint32_t file_size = read_u32_le(header);
        uint32_t name_len  = read_u32_le(header + 4);

        /* Санитарная проверка */
        if (name_len > MAX_PATH_LEN) {
            fprintf(stderr, "Ошибка: недопустимая длина имени файла (%u) в образе\n", name_len);
            break;
        }

        uint8_t salt[SALT_SIZE];
        if (fread(salt, 1, SALT_SIZE, f) != SALT_SIZE) break;

        char *name = NULL;
        if (name_len > 0) {
            name = malloc(name_len + 1);
            if (!name) break;
            if (fread(name, 1, name_len, f) != name_len) {
                free(name);
                break;
            }
            name[name_len] = '\0';
        } else {
            name = strdup("");
        }

        off_t data_offset = ftell(f);

        if (count >= capacity) {
            capacity *= 2;
            ImageRecord *tmp = realloc(records, (size_t)capacity * sizeof(ImageRecord));
            if (!tmp) {
                free(name);
                break;
            }
            records = tmp;
        }

        records[count].file_size   = file_size;
        records[count].name        = name;
        memcpy(records[count].salt, salt, SALT_SIZE);
        records[count].data_offset = data_offset;
        count++;

        /* Пропуск зашифрованного содержимого */
        if (file_size > 0 && fseek(f, (long)file_size, SEEK_CUR) != 0) break;
    }

    fclose(f);
    *out_records = records;
    *out_count   = count;
    return 0;
}

/* === Освобождение массива записей === */

static void free_records(ImageRecord *records, int count) {
    if (!records) return;
    for (int i = 0; i < count; i++) {
        free(records[i].name);
    }
    free(records);
}

/* === Рекурсивный сбор файлов из директории === */

static int collect_dir_files(const char *dir_path, const char *base_dir,
                              char ***input_paths, char ***image_names,
                              int *count, int *capacity, int depth) {
    if (depth > MAX_DEPTH) {
        fprintf(stderr, "Предупреждение: превышена максимальная глубина рекурсии (%d) для %s\n",
                MAX_DEPTH, dir_path);
        return 0;
    }

    DIR *dir = opendir(dir_path);
    if (!dir) {
        fprintf(stderr, "Ошибка: не удалось открыть директорию %s: %s\n",
                dir_path, strerror(errno));
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char full_path[MAX_PATH_LEN];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) {
            fprintf(stderr, "Предупреждение: не удалось получить информацию о %s: %s\n",
                    full_path, strerror(errno));
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            /* Рекурсивный обход поддиректории */
            collect_dir_files(full_path, base_dir, input_paths, image_names,
                              count, capacity, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            /* Добавление обычного файла */
            if (*count >= *capacity) {
                *capacity *= 2;
                *input_paths = realloc(*input_paths, (size_t)*capacity * sizeof(char *));
                *image_names = realloc(*image_names, (size_t)*capacity * sizeof(char *));
                if (!*input_paths || !*image_names) {
                    fprintf(stderr, "Ошибка: не удалось выделить память\n");
                    closedir(dir);
                    return -1;
                }
            }

            (*input_paths)[*count] = strdup(full_path);

            /* Вычисление относительного пути от base_dir */
            size_t base_len = strlen(base_dir);
            while (base_len > 0 && base_dir[base_len - 1] == '/') base_len--;
            const char *rel = full_path + base_len;
            while (*rel == '/') rel++;  /* Пропуск всех ведущих слэшей */
            (*image_names)[*count] = strdup(rel);

            (*count)++;
        }
        /* Символические ссылки и прочие типы файлов пропускаются */
    }

    closedir(dir);
    return 0;
}

/* === Сбор файлов из списка аргументов (файлы и директории) === */

static int collect_files(char **paths, int path_count,
                          char ***input_paths, char ***image_names,
                          int *file_count) {
    int capacity = INIT_CAPACITY;
    *input_paths = malloc((size_t)capacity * sizeof(char *));
    *image_names = malloc((size_t)capacity * sizeof(char *));
    *file_count  = 0;

    if (!*input_paths || !*image_names) {
        fprintf(stderr, "Ошибка: не удалось выделить память\n");
        return -1;
    }

    for (int i = 0; i < path_count; i++) {
        struct stat st;
        if (stat(paths[i], &st) != 0) {
            fprintf(stderr, "Предупреждение: не удалось получить информацию о %s: %s\n",
                    paths[i], strerror(errno));
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            collect_dir_files(paths[i], paths[i], input_paths, image_names,
                              file_count, &capacity, 0);
        } else if (S_ISREG(st.st_mode)) {
            if (*file_count >= capacity) {
                capacity *= 2;
                *input_paths  = realloc(*input_paths,  (size_t)capacity * sizeof(char *));
                *image_names  = realloc(*image_names,  (size_t)capacity * sizeof(char *));
                if (!*input_paths || !*image_names) {
                    fprintf(stderr, "Ошибка: не удалось выделить память\n");
                    return -1;
                }
            }
            (*input_paths)[*file_count]  = strdup(paths[i]);
            (*image_names)[*file_count]  = strdup(paths[i]);
            (*file_count)++;
        } else {
            fprintf(stderr, "Предупреждение: %s не является обычным файлом или директорией\n",
                    paths[i]);
        }
    }

    return 0;
}

/* === Обработка одного файла: шифрование и запись в образ === */

static int process_add_file(AddTask *task) {
    /* Открытие входного файла */
    FILE *fin = fopen(task->input_path, "rb");
    if (!fin) {
        fprintf(stderr, "Ошибка: не удалось открыть файл %s: %s\n",
                task->input_path, strerror(errno));
        return -1;
    }

    /* Открытие образа для записи (каждый поток открывает свой fd) */
    int img_fd = open(task->image_path, O_WRONLY);
    if (img_fd < 0) {
        fprintf(stderr, "Ошибка: не удалось открыть образ %s для записи: %s\n",
                task->image_path, strerror(errno));
        fclose(fin);
        return -1;
    }

    /* Инициализация RC4: ключ = мастер-ключ || соль */
    size_t rc4_key_len = task->master_key_len + SALT_SIZE;
    uint8_t *rc4_key = malloc(rc4_key_len);
    if (!rc4_key) {
        fprintf(stderr, "Ошибка: не удалось выделить память для ключа RC4\n");
        fclose(fin);
        close(img_fd);
        return -1;
    }
    memcpy(rc4_key, task->master_key, task->master_key_len);
    memcpy(rc4_key + task->master_key_len, task->salt, SALT_SIZE);

    secure_rc4_context *ctx = init_secure_rc4(rc4_key, rc4_key_len, task->salt);
    secure_memset(rc4_key, 0, rc4_key_len);
    free(rc4_key);

    if (!ctx) {
        fprintf(stderr, "Ошибка: не удалось инициализировать RC4 контекст для %s\n",
                task->image_name);
        fclose(fin);
        close(img_fd);
        return -1;
    }

    /* Запись заголовка записи: [file_size][name_len][salt] */
    off_t offset = task->write_offset;
    uint32_t name_len = (uint32_t)strlen(task->image_name);

    uint8_t header[8 + SALT_SIZE];
    write_u32_le(header, task->file_size);
    write_u32_le(header + 4, name_len);
    memcpy(header + 8, task->salt, SALT_SIZE);

    ssize_t w = pwrite(img_fd, header, sizeof(header), offset);
    if (w < 0) {
        fprintf(stderr, "Ошибка записи заголовка для %s: %s\n",
                task->image_name, strerror(errno));
        clear_secure_rc4(ctx);
        fclose(fin);
        close(img_fd);
        return -1;
    }
    offset += (off_t)sizeof(header);

    /* Запись имени файла */
    w = pwrite(img_fd, task->image_name, name_len, offset);
    if (w < 0) {
        fprintf(stderr, "Ошибка записи имени файла для %s: %s\n",
                task->image_name, strerror(errno));
        clear_secure_rc4(ctx);
        fclose(fin);
        close(img_fd);
        return -1;
    }
    offset += (off_t)name_len;

    /* Чтение, шифрование и запись содержимого файла по частям (BUFFER_SIZE) */
    uint8_t in_buf[BUFFER_SIZE];
    uint8_t out_buf[BUFFER_SIZE];
    size_t bytes_read;

    while ((bytes_read = fread(in_buf, 1, BUFFER_SIZE, fin)) > 0) {
        if (rc4_crypt(ctx, in_buf, out_buf, bytes_read) != 0) {
            fprintf(stderr, "Ошибка шифрования файла %s\n", task->input_path);
            clear_secure_rc4(ctx);
            fclose(fin);
            close(img_fd);
            return -1;
        }
        w = pwrite(img_fd, out_buf, bytes_read, offset);
        if (w < 0) {
            fprintf(stderr, "Ошибка записи данных для %s: %s\n",
                    task->image_name, strerror(errno));
            clear_secure_rc4(ctx);
            fclose(fin);
            close(img_fd);
            return -1;
        }
        offset += (off_t)bytes_read;
    }

    clear_secure_rc4(ctx);
    fclose(fin);
    close(img_fd);
    return 0;
}

/* Обёртка потока для параллельного добавления */
static void *add_file_thread(void *arg) {
    AddTask *task = (AddTask *)arg;
    task->success = (process_add_file(task) == 0);
    return NULL;
}

/* === Команда -add === */

static int cmd_add(const char *image_path, const uint8_t *master_key,
                    size_t master_key_len, char **paths, int path_count) {

    /* 1. Определяем, существует ли образ, и читаем его записи */
    ImageRecord *existing = NULL;
    int existing_count = 0;
    off_t image_end = 0;

    struct stat img_st;
    int image_exists = (stat(image_path, &img_st) == 0);

    if (image_exists) {
        if (read_image_records(image_path, &existing, &existing_count) != 0) {
            fprintf(stderr, "Ошибка при чтении существующего образа\n");
            return 1;
        }
        image_end = img_st.st_size;
    }

    /* 2. Собираем файлы из аргументов (раскрываем директории) */
    char **input_paths = NULL;
    char **image_names = NULL;
    int file_count = 0;

    if (collect_files(paths, path_count, &input_paths, &image_names, &file_count) != 0) {
        fprintf(stderr, "Ошибка при сборе файлов\n");
        free_records(existing, existing_count);
        return 1;
    }

    if (file_count == 0) {
        fprintf(stderr, "Нет файлов для добавления\n");
        free_records(existing, existing_count);
        return 1;
    }

    /* 3. Фильтрация дубликатов */
    int *valid = malloc((size_t)file_count * sizeof(int));
    if (!valid) {
        fprintf(stderr, "Ошибка: не удалось выделить память\n");
        for (int i = 0; i < file_count; i++) { free(input_paths[i]); free(image_names[i]); }
        free(input_paths); free(image_names);
        free_records(existing, existing_count);
        return 1;
    }
    for (int i = 0; i < file_count; i++) valid[i] = 1;

    for (int i = 0; i < file_count; i++) {
        if (!valid[i]) continue;

        /* Проверка среди существующих записей образа */
        for (int j = 0; j < existing_count; j++) {
            if (strcmp(image_names[i], existing[j].name) == 0) {
                fprintf(stderr, "Пропуск: файл '%s' уже существует в образе\n", image_names[i]);
                valid[i] = 0;
                break;
            }
        }
        if (!valid[i]) continue;

        /* Проверка дубликатов в текущей партии (оставляем первое вхождение) */
        for (int k = i + 1; k < file_count; k++) {
            if (valid[k] && strcmp(image_names[i], image_names[k]) == 0) {
                fprintf(stderr, "Пропуск: дубликат имени '%s'\n", image_names[k]);
                valid[k] = 0;
            }
        }
    }

    /* 4. Формирование задач с предвычисленными позициями записи */
    AddTask *tasks = malloc((size_t)file_count * sizeof(AddTask));
    if (!tasks) {
        fprintf(stderr, "Ошибка: не удалось выделить память для задач\n");
        free(valid);
        for (int i = 0; i < file_count; i++) { free(input_paths[i]); free(image_names[i]); }
        free(input_paths); free(image_names);
        free_records(existing, existing_count);
        return 1;
    }

    off_t offset = image_end;
    int valid_count = 0;

    for (int i = 0; i < file_count; i++) {
        if (!valid[i]) continue;

        struct stat file_st;
        if (stat(input_paths[i], &file_st) != 0) {
            fprintf(stderr, "Предупреждение: не удалось получить информацию о %s: %s\n",
                    input_paths[i], strerror(errno));
            continue;
        }

        strncpy(tasks[valid_count].input_path, input_paths[i], MAX_PATH_LEN - 1);
        tasks[valid_count].input_path[MAX_PATH_LEN - 1] = '\0';
        strncpy(tasks[valid_count].image_name, image_names[i], MAX_PATH_LEN - 1);
        tasks[valid_count].image_name[MAX_PATH_LEN - 1] = '\0';

        tasks[valid_count].file_size = (uint32_t)file_st.st_size;
        tasks[valid_count].write_offset = offset;
        tasks[valid_count].image_path = image_path;
        tasks[valid_count].master_key = master_key;
        tasks[valid_count].master_key_len = master_key_len;
        tasks[valid_count].success = 0;

        if (generate_salt(tasks[valid_count].salt) != 0) {
            fprintf(stderr, "Ошибка генерации соли для %s, пропуск\n", input_paths[i]);
            continue;
        }

        uint32_t name_len = (uint32_t)strlen(image_names[i]);
        offset += (off_t)(8 + SALT_SIZE + name_len + (uint32_t)file_st.st_size);
        valid_count++;
    }

    if (valid_count == 0) {
        fprintf(stderr, "Все файлы являются дубликатами или недоступны, ничего не добавлено\n");
        free(tasks);
        free(valid);
        for (int i = 0; i < file_count; i++) { free(input_paths[i]); free(image_names[i]); }
        free(input_paths); free(image_names);
        free_records(existing, existing_count);
        return 0;
    }

    /* 5. Предварительное расширение (или создание) файла образа */
    {
        int fd;
        if (!image_exists) {
            fd = open(image_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        } else {
            fd = open(image_path, O_WRONLY);
        }
        if (fd < 0) {
            fprintf(stderr, "Ошибка: не удалось открыть образ %s: %s\n",
                    image_path, strerror(errno));
            free(tasks); free(valid);
            for (int i = 0; i < file_count; i++) { free(input_paths[i]); free(image_names[i]); }
            free(input_paths); free(image_names);
            free_records(existing, existing_count);
            return 1;
        }
        if (ftruncate(fd, offset) != 0) {
            fprintf(stderr, "Ошибка: не удалось установить размер образа: %s\n", strerror(errno));
            close(fd);
            free(tasks); free(valid);
            for (int i = 0; i < file_count; i++) { free(input_paths[i]); free(image_names[i]); }
            free(input_paths); free(image_names);
            free_records(existing, existing_count);
            return 1;
        }
        close(fd);
    }

    /* 6. Обработка файлов: последовательная (1 файл) или параллельная (>1) */
    if (valid_count == 1) {
        /* Последовательный режим */
        tasks[0].success = (process_add_file(&tasks[0]) == 0);
    } else {
        /* Параллельный режим: батчи по MAX_THREADS потоков */
        int processed = 0;
        while (processed < valid_count) {
            int batch = valid_count - processed;
            if (batch > MAX_THREADS) batch = MAX_THREADS;

            pthread_t threads[MAX_THREADS];
            for (int i = 0; i < batch; i++) {
                if (pthread_create(&threads[i], NULL, add_file_thread,
                                   &tasks[processed + i]) != 0) {
                    fprintf(stderr, "Ошибка: не удалось создать поток для %s\n",
                            tasks[processed + i].image_name);
                    tasks[processed + i].success = 0;
                    threads[i] = pthread_self();  /* фиктивный поток */
                }
            }
            for (int i = 0; i < batch; i++) {
                if (!pthread_equal(threads[i], pthread_self())) {
                    pthread_join(threads[i], NULL);
                }
            }

            processed += batch;
        }
    }

    /* 7. Отчёт о результатах */
    int added = 0, failed = 0;
    for (int i = 0; i < valid_count; i++) {
        if (tasks[i].success) added++;
        else failed++;
    }

    if (failed > 0) {
        printf("Добавлено файлов: %d, ошибок: %d\n", added, failed);
    } else {
        printf("Добавлено файлов: %d\n", added);
    }

    /* Очистка */
    free(tasks);
    free(valid);
    for (int i = 0; i < file_count; i++) { free(input_paths[i]); free(image_names[i]); }
    free(input_paths); free(image_names);
    free_records(existing, existing_count);

    return (failed > 0) ? 1 : 0;
}

/* === Команда -list === */

static int cmd_list(const char *image_path) {
    ImageRecord *records = NULL;
    int count = 0;

    if (read_image_records(image_path, &records, &count) != 0) {
        fprintf(stderr, "Ошибка при чтении образа\n");
        return 1;
    }

    if (count == 0) {
        printf("Образ пуст или не существует\n");
        return 0;
    }

    /* Сортировка по имени */
    qsort(records, (size_t)count, sizeof(ImageRecord), compare_records_by_name);

    /* Вывод таблицы файлов */
    printf("%-50s %10s\n", "Имя файла", "Размер (байт)");
    printf("--------------------------------------------------  ----------\n");
    for (int i = 0; i < count; i++) {
        printf("%-50s %10u\n", records[i].name, records[i].file_size);
    }

    free_records(records, count);
    return 0;
}

/* === Команда -get === */

static int cmd_get(const char *image_path, const uint8_t *master_key,
                    size_t master_key_len, const char *out_path,
                    const char *file_name) {
    ImageRecord *records = NULL;
    int count = 0;

    if (read_image_records(image_path, &records, &count) != 0) {
        fprintf(stderr, "Ошибка при чтении образа\n");
        return 1;
    }

    /* Поиск файла по имени */
    int found = -1;
    for (int i = 0; i < count; i++) {
        if (strcmp(records[i].name, file_name) == 0) {
            found = i;
            break;
        }
    }

    if (found < 0) {
        fprintf(stderr, "Файл '%s' не найден в образе\n", file_name);
        free_records(records, count);
        return 1;
    }

    /* Инициализация RC4: ключ = мастер-ключ || соль */
    size_t rc4_key_len = master_key_len + SALT_SIZE;
    uint8_t *rc4_key = malloc(rc4_key_len);
    if (!rc4_key) {
        fprintf(stderr, "Ошибка: не удалось выделить память\n");
        free_records(records, count);
        return 1;
    }
    memcpy(rc4_key, master_key, master_key_len);
    memcpy(rc4_key + master_key_len, records[found].salt, SALT_SIZE);

    secure_rc4_context *ctx = init_secure_rc4(rc4_key, rc4_key_len, records[found].salt);
    secure_memset(rc4_key, 0, rc4_key_len);
    free(rc4_key);

    if (!ctx) {
        fprintf(stderr, "Ошибка: не удалось инициализировать RC4 контекст\n");
        free_records(records, count);
        return 1;
    }

    /* Открытие образа для чтения */
    FILE *img = fopen(image_path, "rb");
    if (!img) {
        fprintf(stderr, "Ошибка: не удалось открыть образ для чтения: %s\n",
                strerror(errno));
        clear_secure_rc4(ctx);
        free_records(records, count);
        return 1;
    }

    if (fseek(img, records[found].data_offset, SEEK_SET) != 0) {
        fprintf(stderr, "Ошибка: не удалось найти данные файла в образе\n");
        fclose(img);
        clear_secure_rc4(ctx);
        free_records(records, count);
        return 1;
    }

    /* Открытие выходного файла */
    FILE *fout = fopen(out_path, "wb");
    if (!fout) {
        fprintf(stderr, "Ошибка: не удалось создать выходной файл %s: %s\n",
                out_path, strerror(errno));
        fclose(img);
        clear_secure_rc4(ctx);
        free_records(records, count);
        return 1;
    }

    /* Чтение, дешифрование и запись по частям */
    uint8_t in_buf[BUFFER_SIZE], out_buf[BUFFER_SIZE];
    uint32_t remaining = records[found].file_size;
    int error = 0;

    while (remaining > 0) {
        size_t to_read = (remaining > BUFFER_SIZE) ? BUFFER_SIZE : remaining;
        size_t bytes_read = fread(in_buf, 1, to_read, img);
        if (bytes_read == 0) {
            fprintf(stderr, "Ошибка: неожиданный конец данных в образе\n");
            error = 1;
            break;
        }

        if (rc4_crypt(ctx, in_buf, out_buf, bytes_read) != 0) {
            fprintf(stderr, "Ошибка дешифрования\n");
            error = 1;
            break;
        }

        if (fwrite(out_buf, 1, bytes_read, fout) != bytes_read) {
            fprintf(stderr, "Ошибка записи в выходной файл\n");
            error = 1;
            break;
        }
        remaining -= (uint32_t)bytes_read;
    }

    fclose(fout);
    fclose(img);
    clear_secure_rc4(ctx);
    free_records(records, count);

    if (error) return 1;

    printf("Файл '%s' расшифрован и сохранён в '%s'\n", file_name, out_path);
    return 0;
}

/* === Использование === */

static void print_usage(const char *prog) {
    fprintf(stderr, "Использование:\n");
    fprintf(stderr, "  %s -add -key <ключ> -image <образ> <файл/директория> ...\n", prog);
    fprintf(stderr, "  %s -list -image <образ>\n", prog);
    fprintf(stderr, "  %s -get -image <образ> -key <ключ> -out <файл> <имя_в_образе>\n", prog);
}

/* === Главная функция === */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *command = argv[1];

    if (strcmp(command, "-add") == 0) {
        /* ./secure_copy -add -key <ключ> -image <образ> <файлы/директории...> */
        const char *key   = NULL;
        const char *image = NULL;
        char *files[argc];     /* VLA, достаточно для числа аргументов */
        int file_count = 0;

        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-key") == 0 && i + 1 < argc) {
                key = argv[++i];
            } else if (strcmp(argv[i], "-image") == 0 && i + 1 < argc) {
                image = argv[++i];
            } else {
                files[file_count++] = argv[i];
            }
        }

        if (!key || !image || file_count == 0) {
            fprintf(stderr, "Ошибка: неверные аргументы для -add\n");
            print_usage(argv[0]);
            return 1;
        }

        size_t key_len = strlen(key);
        if (key_len + SALT_SIZE > RC4_KEY_MAX_SIZE) {
            fprintf(stderr, "Ошибка: ключ слишком длинный (максимум %d байт)\n",
                    RC4_KEY_MAX_SIZE - SALT_SIZE);
            return 1;
        }

        return cmd_add(image, (const uint8_t *)key, key_len, files, file_count);

    } else if (strcmp(command, "-list") == 0) {
        /* ./secure_copy -list -image <образ> */
        const char *image = NULL;

        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-image") == 0 && i + 1 < argc) {
                image = argv[++i];
            }
        }

        if (!image) {
            fprintf(stderr, "Ошибка: не указан образ\n");
            print_usage(argv[0]);
            return 1;
        }

        return cmd_list(image);

    } else if (strcmp(command, "-get") == 0) {
        /* ./secure_copy -get -image <образ> -key <ключ> -out <файл> <имя_в_образе> */
        const char *image    = NULL;
        const char *key      = NULL;
        const char *out_file = NULL;
        const char *file_name = NULL;

        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-image") == 0 && i + 1 < argc) {
                image = argv[++i];
            } else if (strcmp(argv[i], "-key") == 0 && i + 1 < argc) {
                key = argv[++i];
            } else if (strcmp(argv[i], "-out") == 0 && i + 1 < argc) {
                out_file = argv[++i];
            } else {
                file_name = argv[i];
            }
        }

        if (!image || !key || !out_file || !file_name) {
            fprintf(stderr, "Ошибка: неверные аргументы для -get\n");
            print_usage(argv[0]);
            return 1;
        }

        size_t key_len = strlen(key);
        if (key_len + SALT_SIZE > RC4_KEY_MAX_SIZE) {
            fprintf(stderr, "Ошибка: ключ слишком длинный (максимум %d байт)\n",
                    RC4_KEY_MAX_SIZE - SALT_SIZE);
            return 1;
        }

        return cmd_get(image, (const uint8_t *)key, key_len, out_file, file_name);

    } else {
        fprintf(stderr, "Неизвестная команда: %s\n", command);
        print_usage(argv[0]);
        return 1;
    }
}
