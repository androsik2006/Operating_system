# Компилятор и флаги
CC = gcc
CFLAGS = -Wall -Wextra -pedantic -fPIC -pthread -g -D_GNU_SOURCE
LDFLAGS = -ldl -pthread

# Имена файлов
LIB_SRC = libcaesar.c
LIB_HDR = libcaesar.h
LIB_SO = libcaesar.so

TEST_LOADER_SRC = test_loader.c
TEST_LOADER_BIN = test_loader

THREAD_FILE_SRC = thread_file.c
THREAD_FILE_BIN = secure_copy

# Тестовые файлы
INPUT_FILE = input.txt
OUTPUT_FILE = output.txt
DECRYPTED_FILE = decrypted.txt

# Цели по умолчанию
all: $(LIB_SO) $(TEST_LOADER_BIN) $(THREAD_FILE_BIN)

# Сборка динамической библиотеки шифрования
$(LIB_SO): $(LIB_SRC) $(LIB_HDR)
	$(CC) $(CFLAGS) -shared -o $(LIB_SO) $(LIB_SRC)

# Сборка тестового загрузчика (одиночное шифрование)
$(TEST_LOADER_BIN): $(TEST_LOADER_SRC) $(LIB_HDR)
	$(CC) $(CFLAGS) -o $(TEST_LOADER_BIN) $(TEST_LOADER_SRC) $(LDFLAGS)

# Сборка многопоточной программы secure_copy (Задание 3)
$(THREAD_FILE_BIN): $(THREAD_FILE_SRC) $(LIB_HDR)
	$(CC) $(CFLAGS) -o $(THREAD_FILE_BIN) $(THREAD_FILE_SRC) $(LDFLAGS)

# Тест библиотеки шифрования (Задание 1)
test_lib: $(LIB_SO) $(TEST_LOADER_BIN)
	@echo "🧪 Тест шифрования Цезарем..."
	@echo "Hello, student MIREA! Это тест." > $(INPUT_FILE)
	@./$(TEST_LOADER_BIN) ./$(LIB_SO) 65 $(INPUT_FILE) $(OUTPUT_FILE)
	@echo "📄 Зашифровано:"
	@xxd $(OUTPUT_FILE) | head -5
	@./$(TEST_LOADER_BIN) ./$(LIB_SO) -65 $(OUTPUT_FILE) $(DECRYPTED_FILE)
	@echo "📄 Расшифровано:"
	@cat $(DECRYPTED_FILE)
	@diff -q $(INPUT_FILE) $(DECRYPTED_FILE) && echo "✅ Тест пройден" || echo "❌ Ошибка"

# Тест многопоточного копирования (Задание 3 - основной)
test_threads: $(LIB_SO) $(THREAD_FILE_BIN)
	@echo "🧵 Тест многопоточной обработки (5 файлов, 3 потока)..."
	@for i in 1 2 3 4 5; do \
		echo "Файл №$$i: контент для шифрования потоком" > file$$i.txt; \
	done
	@echo "🚀 Запуск: ./secure_copy file{1..5}.txt outdir/65"
	@./$(THREAD_FILE_BIN) file1.txt file2.txt file3.txt file4.txt file5.txt outdir/65
	@echo ""
	@echo "📁 Выходные файлы:"
	@ls -la outdir/ 2>/dev/null || echo "❌ outdir не создан"
	@echo ""
	@echo "📋 Лог операций (log.txt):"
	@cat log.txt 2>/dev/null || echo "⚠️ log.txt не найден"
	@echo ""
	@echo "🔍 Проверка целостности (дешифровка file1):"
	@./$(TEST_LOADER_BIN) ./$(LIB_SO) -65 outdir/file1.txt check.txt 2>/dev/null
	@cat check.txt 2>/dev/null
	@rm -f check.txt

# Проверка на гонки данных (valgrind/helgrind)
check_races: $(LIB_SO) $(THREAD_FILE_BIN)
	@echo "🔍 Проверка на race conditions (valgrind --tool=helgrind)..."
	@echo "test data" > race_test.txt
	valgrind --tool=helgrind --error-exitcode=1 \
		./$(THREAD_FILE_BIN) race_test.txt race_test.txt race_test.txt test_race/42 2>&1 | tail -20
	@rm -f race_test.txt
	@rm -rf test_race/

# Демонстрация для сдачи задания
demo: clean all test_threads
	@echo ""
	@echo "=========================================="
	@echo "✅ ДЕМО ГОТОВА К СДАЧЕ"
	@echo "=========================================="
	@echo "📊 Статистика из лога:"
	@grep -c "SUCCESS" log.txt 2>/dev/null && echo "файлов успешно обработано" || echo "0"
	@echo ""
	@echo "📋 Последние 10 записей лога:"
	@tail -10 log.txt

# Установка библиотеки в систему
install: $(LIB_SO)
	sudo cp $(LIB_SO) /usr/local/lib/
	sudo ldconfig
	@echo "✅ $(LIB_SO) установлен в /usr/local/lib/"

# Полная очистка
clean:
	rm -f $(LIB_SO) $(TEST_LOADER_BIN) $(THREAD_FILE_BIN)
	rm -f $(INPUT_FILE) $(OUTPUT_FILE) $(DECRYPTED_FILE)
	rm -f file*.txt check.txt race_test.txt
	rm -rf outdir/ test_race/ test_out/
	rm -f log.txt
	@echo "🧹 Очистка завершена"

# Быстрая пересборка
rebuild: clean all

.PHONY: all test_lib test_threads check_races demo install clean rebuild
