CC = gcc
CFLAGS = -Wall -Wextra -pedantic -fPIC -pthread -lrt  # Добавлен -pthread и -lrt для работы с потоками и временем
LIB_NAME = libcaesar.so
TEST_PROG = test_loader

all: $(LIB_NAME) $(TEST_PROG)  # Теперь собираем и библиотеку, и тестовую программу

$(LIB_NAME): libcaesar.c
	$(CC) $(CFLAGS) -shared -o $(LIB_NAME) libcaesar.c

$(TEST_PROG): test_loader.c
	$(CC) $(CFLAGS) -o $(TEST_PROG) test_loader.c -ldl -pthread  # Добавлен -pthread для тестовой программы

install: $(LIB_NAME)
	sudo cp $(LIB_NAME) /usr/local/lib/
	sudo ldconfig

test: $(LIB_NAME) $(TEST_PROG)
	# Создаем тестовый файл если нет
	echo "Hello, student MIREA" > input.txt

	# Тестирование последовательного режима
	echo "=== Последовательный режим ==="
	time ./$(TEST_PROG) seq $(LIB_NAME) 65 input.txt output.txt  # Добавлен флаг 'seq' для последовательного режима
	@echo "--- Результат шифрования (output.txt) ---"
	xxd output.txt

	# Тестирование параллельного режима
	echo "=== Параллельный режим ==="
	time ./$(TEST_PROG) par $(LIB_NAME) 65 input.txt output.txt  # Добавлен флаг 'par' для параллельного режима
	@echo "--- Результат шифрования (output.txt) ---"
	xxd output.txt

	# Расшифровка обратно
	echo "--- Расшифровка обратно ---"
	./$(TEST_PROG) $(LIB_NAME) 65 output.txt decrypted.txt
	cat decrypted.txt

clean:
	rm -f $(LIB_NAME) $(TEST_PROG) input.txt output.txt decrypted.txt
