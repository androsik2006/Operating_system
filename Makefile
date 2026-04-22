CC = gcc
CFLAGS = -Wall -Wextra -pedantic -fPIC -pthread -lrt
LIB_NAME = libcaesar.so
TEST_PROG = test_loader
THREAD_PROG = secure_copy

all: $(LIB_NAME) $(TEST_PROG) $(THREAD_PROG)

$(LIB_NAME): libcaesar.c
	$(CC) $(CFLAGS) -shared -o $(LIB_NAME) libcaesar.c

$(TEST_PROG): test_loader.c
	$(CC) $(CFLAGS) -o $(TEST_PROG) test_loader.c -ldl -pthread

$(THREAD_PROG): thread_file.c
	$(CC) $(CFLAGS) -o $(THREAD_PROG) thread_file.c -ldl -pthread

install: $(LIB_NAME)
	sudo cp $(LIB_NAME) /usr/local/lib/
	sudo ldconfig

test: $(LIB_NAME) $(TEST_PROG) $(THREAD_PROG)
	# Создаем тестовые файлы
	@echo "Создание тестовых файлов..."
	echo "Привет, студентка МИРЕА" > input.txt
	for i in 1 2 3 4 5 6 7 8 9 10; do \
		echo "Тестовый файл номер $$i с некоторым содержимым для шифрования" > test_file_$$i.txt; \
	done
	
	# Тестирование test_loader
	@echo ""
	@echo "=== Тестирование test_loader ==="
	./$(TEST_PROG) $(LIB_NAME) 65 input.txt output.txt
	@echo "Зашифровано: output.txt"
	./$(TEST_PROG) $(LIB_NAME) 65 output.txt decrypted.txt
	@echo "Расшифровано: decrypted.txt"
	@echo "Результат:"
	cat decrypted.txt
	
	# Тестирование последовательного режима
	@echo ""
	@echo "=== Последовательный режим (3 файла) ==="
	time ./$(THREAD_PROG) $(LIB_NAME) 65 --mode=sequential test_file_1.txt test_file_2.txt test_file_3.txt
	
	# Тестирование параллельного режима
	@echo ""
	@echo "=== Параллельный режим (10 файлов) ==="
	time ./$(THREAD_PROG) $(LIB_NAME) 65 --mode=parallel test_file_1.txt test_file_2.txt test_file_3.txt test_file_4.txt test_file_5.txt test_file_6.txt test_file_7.txt test_file_8.txt test_file_9.txt test_file_10.txt
	
	# Тестирование автоматического режима
	@echo ""
	@echo "=== Автоматический режим (3 файла - будет sequential) ==="
	./$(THREAD_PROG) $(LIB_NAME) 65 --mode=auto test_file_1.txt test_file_2.txt test_file_3.txt
	
	@echo ""
	@echo "=== Автоматический режим (10 файлов - будет parallel) ==="
	./$(THREAD_PROG) $(LIB_NAME) 65 --mode=auto test_file_1.txt test_file_2.txt test_file_3.txt test_file_4.txt test_file_5.txt test_file_6.txt test_file_7.txt test_file_8.txt test_file_9.txt test_file_10.txt

clean:
	rm -f $(LIB_NAME) $(TEST_PROG) $(THREAD_PROG)
	rm -f input.txt output.txt decrypted.txt
	rm -f test_file_*.txt
	rm -f encrypted_test_file_*.txt

.PHONY: all install test clean
