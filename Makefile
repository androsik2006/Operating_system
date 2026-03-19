# Makefile для secure_copy
# Сборка многопоточного шифровальщика с синхронизацией

CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -g
LDFLAGS = -pthread -ldl

# Целевые файлы
TARGET = secure_copy
LIB = libcaesar.so
MAIN_SRC = secure_copy.c
LIB_SRC = libcaesar.c
HEADERS = libcaesar.h

.PHONY: all clean test test_threads check_races demo help

all: $(LIB) $(TARGET)

# Сборка динамической библиотеки шифрования
$(LIB): $(LIB_SRC) $(HEADERS)
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $<

# Сборка основного исполняемого файла
$(TARGET): $(MAIN_SRC) $(HEADERS) $(LIB)
	$(CC) $(CFLAGS) -o $@ $(MAIN_SRC) -L. -lcaesar -Wl,-rpath,. $(LDFLAGS)

# Тест: единичное шифрование
test: $(TARGET)
	@echo "=== Тест: единичное шифрование ==="
	@echo "Test data for encryption" > test_input.txt
	./$(TARGET) test_input.txt output/7
	@echo "Проверка результата:"
	@ls -la output/ 2>/dev/null || echo "Выходная директория не создана"
	@rm -f test_input.txt

# Тест: многопоточная обработка (минимум 5 файлов)
test_threads: $(TARGET)
	@echo "=== Тест: многопоточная обработка 5 файлов ==="
	@mkdir -p test_inputs output_test
	@for i in 1 2 3 4 5; do echo "Content of file $$i" > test_inputs/file$$i.txt; done
	./$(TARGET) test_inputs/file*.txt output_test/K
	@echo "Результаты в output_test/:"
	@ls -1 output_test/
	@echo "Содержимое log.txt:"
	@cat log.txt
	@rm -rf test_inputs output_test

# Проверка на гонки данных (требуется valgrind с helgrind)
check_races: $(TARGET)
	@echo "=== Проверка на гонки данных (helgrind) ==="
	@which valgrind >/dev/null 2>&1 || { echo "valgrind не установлен"; exit 1; }
	@mkdir -p race_test_out
	@for i in 1 2 3; do echo "Race test $$i" > race_$$i.txt; done
	valgrind --tool=helgrind --log-file=helgrind.log ./$(TARGET) race_*.txt race_test_out/X
	@echo "Отчёт helgrind:"
	@cat helgrind.log | grep -E "(ERROR|WARNING|no errors)" || echo "Проверка завершена"
	@rm -f race_*.txt helgrind.log
	@rm -rf race_test_out

# Демо для защиты: 5+ файлов с показом лога
demo: $(TARGET)
	@echo "=== Демо для защиты ==="
	@mkdir -p demo_in demo_out
	@for i in A B C D E F; do echo "Demo content $$i - $(date)" > demo_in/demo_$$i.txt; done
	@echo "Запуск ./$(TARGET) demo_in/*.txt demo_out/0x5A"
	./$(TARGET) demo_in/demo_*.txt demo_out/0x5A
	@echo -e "\n=== Выходные файлы ==="
	@ls -la demo_out/
	@echo -e "\n=== Содержимое log.txt (последние 15 строк) ==="
	@tail -15 log.txt
	@rm -rf demo_in demo_out

# Очистка
clean:
	rm -f $(TARGET) $(LIB) *.o log.txt helgrind.log
	rm -rf output/ output_test/ race_test_out/ demo_in/ demo_out/

# Справка
help:
	@echo "Цели Makefile:"
	@echo "  all           - Сборка проекта (по умолчанию)"
	@echo "  test          - Тест единичного шифрования"
	@echo "  test_threads  - Тест многопоточной обработки (5 файлов)"
	@echo "  check_races   - Проверка на гонки данных через helgrind"
	@echo "  demo          - Демо для защиты (5+ файлов + лог)"
	@echo "  clean         - Удаление собранных файлов"
	@echo "  help          - Эта справка"
