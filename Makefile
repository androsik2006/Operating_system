CC = gcc
CFLAGS = -Wall -Wextra -pedantic -fPIC -pthread -D_GNU_SOURCE
LDFLAGS = -lrt
FUSE_CFLAGS = \$(shell pkg-config fuse3 --cflags)
FUSE_LDFLAGS = \$(shell pkg-config fuse3 --libs)
LIB_NAME = librc4_container.so
CONTAINER_PROG = container_tool
FUSE_PROG = fuse_reader
TEST_PROG = test_rc4_container

# Параметры для тестирования
NUM_TEST_FILES = 10
MAX_THREADS = 4  # < 5 потоков, как требуется
TEST_KEY = "my_secret_key"
TEST_SALT = "16byte_salt_here"

# Главная цель (default target)
all: $(LIB_NAME) $(CONTAINER_PROG) $(FUSE_PROG) $(TEST_PROG)

# Правило сборки библиотеки
\$(LIB_NAME): rc4_crypto.c container_format.c
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -shared -o $@ $^ $(LDFLAGS) \$(FUSE_LDFLAGS)

# Правило сборки утилит
$(CONTAINER_PROG): container_tool.c $(LIB_NAME)
	$(CC) $(CFLAGS) -o $@ $< -L. -lrc4_container -ldl -pthread \$(LDFLAGS)

$(FUSE_PROG): fuse_reader.c $(LIB_NAME)
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -o $@ $< -L. -lrc4_container $(LDFLAGS) \$(FUSE_LDFLAGS)

$(TEST_PROG): test_rc4.c $(LIB_NAME)
	$(CC) $(CFLAGS) -o $@ $< -L. -lrc4_container -ldl -pthread \$(LDFLAGS)

# Установка
install: $(LIB_NAME) $(CONTAINER_PROG) \$(FUSE_PROG)
	@echo "Установка:"
	sudo cp \$(LIB_NAME) /usr/local/lib/
	sudo ldconfig
	sudo cp \$(CONTAINER_PROG) /usr/local/bin/
	sudo cp \$(FUSE_PROG) /usr/local/bin/

# Создание тестовых данных
create_test_files:
	@echo "Создание тестовых файлов (включая вложенность)..."
	mkdir -p test_dir/subdir
	echo "Привет, контейнер с RC4" > input.txt
	echo "Файл во вложенной директории" > test_dir/subdir/nested_file.txt
	for i in $(shell seq 1 $(NUM_TEST_FILES)); do \
		echo "Тестовый файл $$i для контейнера RC4" > test_file_$$i.txt; \
	done

# Тестирование
test: create_test_files $(LIB_NAME) $(CONTAINER_PROG) $(FUSE_PROG) $(TEST_PROG)
	@echo ""
	@echo "=== Тестирование добавления файлов в контейнер (параллельно, <5 потоков) ==="
	./$(CONTAINER_PROG) --add --threads=$(MAX_THREADS) --key="$(TEST_KEY)" --salt="$(TEST_SALT)" \
		input.txt test_file_1.txt test_dir/subdir/nested_file.txt output.container
	# ... (остальные команды теста)

# Очистка
clean:
	rm -f $(LIB_NAME) $(CONTAINER_PROG) $(FUSE_PROG) $(TEST_PROG)
	rm -f input.txt output.container extracted_input.txt
	rm -rf test_dir mountpoint
	rm -f test_file_*.txt

# Фиктивные цели (чтобы make не пытался их компилировать)
.PHONY: all install test clean create_test_files
