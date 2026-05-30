# === Глобальные настройки ===
CC = gcc
CFLAGS = -Wall -Wextra -pedantic -fPIC -pthread -D_GNU_SOURCE -O2
LDFLAGS = -lrt -ldl -pthread
LIB_NAME = libcaesar.so

# === Целевые программы ===
LIB_TARGET = $(LIB_NAME)
TEST_LOADER = test_loader
THREAD_TOOL = thread_file
DISK_IMG_TOOL = disk_image_tool

# === Главная цель ===
all: $(LIB_TARGET) $(TEST_LOADER) $(THREAD_TOOL) $(DISK_IMG_TOOL)

# === Сборка библиотеки (динамическая) ===
$(LIB_TARGET): libcaesar.c libcaesar.h
	$(CC) $(CFLAGS) -shared -o $@ libcaesar.c $(LDFLAGS)

# === Тестовый загрузчик библиотеки ===
$(TEST_LOADER): test_loader.c libcaesar.h
	$(CC) $(CFLAGS) -o $@ test_loader.c -L. -lcaesar -Wl,-rpath,. $(LDFLAGS)

# === Многопоточный обработчик файлов ===
$(THREAD_TOOL): thread_file.c libcaesar.h $(LIB_TARGET)
	$(CC) $(CFLAGS) -o $@ thread_file.c -L. -lcaesar -Wl,-rpath,. $(LDFLAGS)

# === Утилита создания образов диска (НОВАЯ) ===
$(DISK_IMG_TOOL): disk_image.c
	$(CC) $(CFLAGS) -DDISK_IMAGE_STANDALONE -o $@ disk_image.c $(LDFLAGS)

# === Тестирование ===
test: all
	@echo "=== Тест библиотеки ==="
	./$(TEST_LOADER) ./$(LIB_TARGET) "4d79536563726574" "53616c7431323334353637383930" input.txt output.txt
	@echo ""
	@echo "=== Тест многопоточности ==="
	./$(THREAD_TOOL) ./$(LIB_TARGET) "4d79536563726574" "53616c7431323334353637383930" "container.dat" "--mode=sequential" test_file_1.txt test_file_2.txt
	@echo ""
	@echo "=== Тест создания образа ==="
	./$(DISK_IMG_TOOL) test_disk.img input.txt test_file_1.txt

# === Создание тестовых файлов ===
create_test_files:
	@echo "Создание тестовых файлов..."
	@echo "Привет, студентка МИРЕА" > input.txt
	@for i in $$(seq 1 10); do \
		echo "Тестовый файл номер $$i с некоторым содержимым для шифрования" > test_file_$$i.txt; \
	done

# === Очистка ===
clean:
	rm -f $(LIB_TARGET) $(TEST_LOADER) $(THREAD_TOOL) $(DISK_IMG_TOOL)
	rm -f *.img output.txt decrypted.txt container.dat
	rm -f test_file_*.txt input.txt

# === Установка (опционально) ===
install: all
	@echo "Установка в /usr/local..."
	sudo cp $(LIB_TARGET) /usr/local/lib/
	sudo ldconfig
	sudo cp $(TEST_LOADER) $(THREAD_TOOL) $(DISK_IMG_TOOL) /usr/local/bin/

.PHONY: all test clean create_test_files install
