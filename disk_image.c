/**
 * disk_image.c - Утилита для создания образов диска (raw + FAT12/FAT16)
 * Для учебных целей по операционным системам
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

// === Константы ===
#define SECTOR_SIZE         512
#define BOOT_SECTOR_COUNT   1
#define FAT_COUNT           2
#define ROOT_DIR_ENTRIES    224      // Для FAT16
#define MAX_FILENAME_LEN    12

// === Структуры ===

// Загрузочная запись (MBR/Boot sector)
#pragma pack(push, 1)
typedef struct {
    uint8_t jump[3];              // 0xEB 0x3C 0x90
    char oem_name[8];             // "MSWIN4.1"
    
    // BPB (BIOS Parameter Block)
    uint16_t bytes_per_sector;    // 512
    uint8_t sectors_per_cluster;  // 1, 2, 4, 8...
    uint16_t reserved_sectors;    // обычно 1
    uint8_t fat_count;            // 2
    uint16_t root_entries;        // 224 для FAT16
    uint16_t total_sectors_16;    // <65536
    uint8_t media_type;           // 0xF8 = жесткий диск
    uint16_t sectors_per_fat;     // размер одной FAT
    uint16_t sectors_per_track;   // геометрия
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;    // если total_sectors_16 == 0
    
    // EBPB (Extended BIOS Parameter Block)
    uint8_t drive_number;         // 0x80
    uint8_t reserved;
    uint8_t boot_signature;       // 0x29
    uint32_t volume_id;           // серийный номер тома
    char volume_label[11];        // "NO NAME    "
    char fs_type[8];              // "FAT16   "
    
    uint8_t boot_code[448];       // код загрузчика
    uint16_t boot_signature_end;  // 0xAA55
} BootSector;
#pragma pack(pop)

// Запись в корневом каталоге
#pragma pack(push, 1)
typedef struct {
    char filename[8];
    char extension[3];
    uint8_t attributes;           // 0x20 = архивный
    uint8_t reserved;
    uint8_t create_time_tenths;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t high_cluster;        // для FAT32
    uint16_t modify_time;
    uint16_t modify_date;
    uint16_t first_cluster;       // первый кластер файла
    uint32_t file_size;
} DirEntry;
#pragma pack(pop)

// === Глобальные настройки образа ===
typedef struct {
    uint32_t total_sectors;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint16_t sectors_per_fat;
    uint16_t root_entries;
    char volume_label[12];
} ImageConfig;

// === Вспомогательные функции ===

// Конвертация строки в формат 8.3 (FAT)
void to_fat_name(const char* src, char* dst) {
    memset(dst, ' ', 11);
    
    const char* dot = strchr(src, '.');
    int name_len = dot ? (dot - src) : strlen(src);
    if (name_len > 8) name_len = 8;
    memcpy(dst, src, name_len);
    
    // Приводим к верхнему регистру
    for (int i = 0; i < 11; i++) {
        if (dst[i] >= 'a' && dst[i] <= 'z')
            dst[i] -= 32;
        else if (dst[i] == '.')
            dst[i] = ' ';
    }
    
    if (dot) {
        int ext_len = strlen(dot + 1);
        if (ext_len > 3) ext_len = 3;
        memcpy(dst + 8, dot + 1, ext_len);
        for (int i = 8; i < 11; i++) {
            if (dst[i] >= 'a' && dst[i] <= 'z')
                dst[i] -= 32;
        }
    }
}

// Получение текущего времени в формате FAT
uint16_t get_fat_time() {
    time_t t = time(NULL);
    struct tm* tm_info = localtime(&t);
    return (tm_info->tm_hour << 11) | (tm_info->tm_min << 5) | (tm_info->tm_sec / 2);
}

uint16_t get_fat_date() {
    time_t t = time(NULL);
    struct tm* tm_info = localtime(&t);
    return ((tm_info->tm_year + 1900 - 1980) << 9) | 
           ((tm_info->tm_mon + 1) << 5) | 
           tm_info->tm_mday;
}

// === Создание образа ===

int create_disk_image(const char* filename, ImageConfig* cfg, const char** files, int file_count) {
    FILE* img = fopen(filename, "wb");
    if (!img) {
        perror("fopen");
        return -1;
    }
    
    // 1. Инициализация загрузочного сектора
    BootSector boot = {0};
    memcpy(boot.jump, "\xEB\x3C\x90", 3);
    memcpy(boot.oem_name, "MSWIN4.1", 8);
    
    boot.bytes_per_sector = SECTOR_SIZE;
    boot.sectors_per_cluster = cfg->sectors_per_cluster;
    boot.reserved_sectors = cfg->reserved_sectors;
    boot.fat_count = FAT_COUNT;
    boot.root_entries = cfg->root_entries;
    boot.total_sectors_16 = cfg->total_sectors;
    boot.media_type = 0xF8;
    boot.sectors_per_fat = cfg->sectors_per_fat;
    boot.sectors_per_track = 63;
    boot.heads = 255;
    boot.drive_number = 0x80;
    boot.boot_signature = 0x29;
    boot.volume_id = 0x12345678;
    
    char vol_label[12] = {0};
    snprintf(vol_label, sizeof(vol_label), "%-11s", cfg->volume_label);
    memcpy(boot.volume_label, vol_label, 11);
    memcpy(boot.fs_type, "FAT16   ", 8);
    
    boot.boot_signature_end = 0xAA55;
    
    // Запись загрузочного сектора
    fseek(img, 0, SEEK_SET);
    fwrite(&boot, sizeof(BootSector), 1, img);
    
    // 2. Инициализация и запись FAT таблиц
    uint16_t* fat = calloc(cfg->sectors_per_fat * SECTOR_SIZE / sizeof(uint16_t), sizeof(uint16_t));
    if (!fat) { fclose(img); return -1; }
    
    // Первые 2 записи зарезервированы
    fat[0] = 0xFFF8;  // медиа-дескриптор
    fat[1] = 0xFFFF;  // конец цепочки для root
    
    // Расчёт смещений
    uint32_t fat_start = cfg->reserved_sectors;
    uint32_t root_dir_start = fat_start + FAT_COUNT * cfg->sectors_per_fat;
    uint32_t data_start = root_dir_start + (cfg->root_entries * sizeof(DirEntry) + SECTOR_SIZE - 1) / SECTOR_SIZE;
    uint32_t next_cluster = 2;  // первый доступный кластер данных
    
    // 3. Запись файлов в образ
    DirEntry* root_entries = calloc(cfg->root_entries, sizeof(DirEntry));
    int entry_idx = 0;
    
    for (int f = 0; f < file_count && entry_idx < cfg->root_entries; f++) {
        FILE* src = fopen(files[f], "rb");
        if (!src) {
            fprintf(stderr, "Не удалось открыть: %s\n", files[f]);
            continue;
        }
        
        // Получаем размер файла
        fseek(src, 0, SEEK_END);
        uint32_t file_size = ftell(src);
        fseek(src, 0, SEEK_SET);
        
        // Вычисляем количество кластеров
        uint32_t clusters_needed = (file_size + cfg->sectors_per_cluster * SECTOR_SIZE - 1) / 
                                   (cfg->sectors_per_cluster * SECTOR_SIZE);
        if (clusters_needed == 0) clusters_needed = 1;
        
        // Заполняем запись каталога
        DirEntry* entry = &root_entries[entry_idx++];
        char fat_name[12];
        to_fat_name(strrchr(files[f], '/') ? strrchr(files[f], '/') + 1 : files[f], fat_name);
        memcpy(entry->filename, fat_name, 8);
        memcpy(entry->extension, fat_name + 8, 3);
        entry->attributes = 0x20;  // архивный
        entry->create_time = get_fat_time();
        entry->create_date = get_fat_date();
        entry->modify_time = entry->create_time;
        entry->modify_date = entry->create_date;
        entry->first_cluster = next_cluster;
        entry->file_size = file_size;
        
        // Запись данных файла
        uint32_t data_offset = (data_start + (next_cluster - 2) * cfg->sectors_per_cluster) * SECTOR_SIZE;
        fseek(img, data_offset, SEEK_SET);
        
        uint8_t buffer[SECTOR_SIZE];
        uint32_t remaining = file_size;
        uint16_t prev_cluster = next_cluster;
        
        while (remaining > 0) {
            size_t to_read = (remaining > SECTOR_SIZE) ? SECTOR_SIZE : remaining;
            fread(buffer, 1, to_read, src);
            if (to_read < SECTOR_SIZE) memset(buffer + to_read, 0, SECTOR_SIZE - to_read);
            fwrite(buffer, 1, SECTOR_SIZE, img);
            remaining -= to_read;
        }
        
        // Обновление FAT: создаём цепочку кластеров
        for (uint32_t i = 0; i < clusters_needed; i++) {
            uint32_t current = next_cluster + i;
            if (i == clusters_needed - 1) {
                fat[current] = 0xFFFF;  // конец файла
            } else {
                fat[current] = current + 1;
            }
        }
        next_cluster += clusters_needed;
        
        fclose(src);
        printf("Добавлен: %s (%d байт, кластер %d)\n", files[f], file_size, root_entries[entry_idx-1].first_cluster);
    }
    
    // 4. Запись корневого каталога
    fseek(img, root_dir_start * SECTOR_SIZE, SEEK_SET);
    fwrite(root_entries, sizeof(DirEntry), entry_idx, img);
    
    // 5. Запись FAT таблиц (2 копии)
    for (int fat_copy = 0; fat_copy < FAT_COUNT; fat_copy++) {
        fseek(img, (fat_start + fat_copy * cfg->sectors_per_fat) * SECTOR_SIZE, SEEK_SET);
        fwrite(fat, cfg->sectors_per_fat * SECTOR_SIZE, 1, img);
    }
    
    // 6. Заполнение образа нулями до конца (опционально)
    // Можно добавить, если нужен фиксированный размер
    
    // Очистка
    free(fat);
    free(root_entries);
    fclose(img);
    
    printf("✓ Образ создан: %s (%d секторов, %d МБ)\n", 
           filename, cfg->total_sectors, cfg->total_sectors * SECTOR_SIZE / 1024 / 1024);
    return 0;
}

// === Конфигурация по умолчанию для образа 1.44 МБ (флоппи) ===
ImageConfig get_floppy_config() {
    ImageConfig cfg = {
        .total_sectors = 2880,           // 1.44 МБ
        .sectors_per_cluster = 1,
        .reserved_sectors = 1,
        .sectors_per_fat = 9,
        .root_entries = 224,
        .volume_label = "MYFLOPPY"
    };
    return cfg;
}

// === Конфигурация для образа 10 МБ ===
ImageConfig get_small_hdd_config() {
    ImageConfig cfg = {
        .total_sectors = 20480,          // 10 МБ
        .sectors_per_cluster = 4,
        .reserved_sectors = 1,
        .sectors_per_fat = 20,
        .root_entries = 224,
        .volume_label = "MYHDD   "
    };
    return cfg;
}

// === main для тестирования ===
#ifdef DISK_IMAGE_STANDALONE
int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Использование: %s <образ.img> <файл1> [файл2] ...\n", argv[0]);
        return 1;
    }
    
    ImageConfig cfg = get_floppy_config();  // или get_small_hdd_config()
    const char** files = (const char**)(argv + 2);
    int file_count = argc - 2;
    
    return create_disk_image(argv[1], &cfg, files, file_count);
}
#endif
