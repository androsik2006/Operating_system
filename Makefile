CC = gcc
CFLAGS = -Wall -Wextra -pedantic -pthread -D_GNU_SOURCE
LIB_NAME = libcaesar.so
THREAD_PROG = secure_copy

all: $(LIB_NAME) $(THREAD_PROG)

$(LIB_NAME): libcaesar.c libcaesar.h
	$(CC) $(CFLAGS) -fPIC -shared -o $(LIB_NAME) libcaesar.c

$(THREAD_PROG): thread_file.c libcaesar.c libcaesar.h
	$(CC) $(CFLAGS) -o $(THREAD_PROG) thread_file.c libcaesar.c -pthread

test: $(THREAD_PROG)
	@echo "=== Создание тестовых файлов и директорий ==="
	@mkdir -p test_dir/sub1/sub2/sub3/sub4
	@echo "Привет, мир!" > test_file1.txt
	@echo "Тестовый файл 2" > test_file2.txt
	@echo "Файл в корне директории" > test_dir/file_root.txt
	@echo "Уровень вложенности 1" > test_dir/sub1/deep1.txt
	@echo "Уровень вложенности 2" > test_dir/sub1/sub2/deep2.txt
	@echo "Уровень вложенности 3" > test_dir/sub1/sub2/sub3/deep3.txt
	@echo "Уровень вложенности 4" > test_dir/sub1/sub2/sub3/sub4/deep4.txt
	@echo ""
	@echo "=== Тест 1: Создание нового образа и добавление файлов ==="
	./$(THREAD_PROG) -add -key "secret" -image test.img test_file1.txt test_file2.txt test_dir/
	@echo ""
	@echo "=== Тест 2: Просмотр списка файлов ==="
	./$(THREAD_PROG) -list -image test.img
	@echo ""
	@echo "=== Тест 3: Попытка добавить дубликат ==="
	./$(THREAD_PROG) -add -key "secret" -image test.img test_file1.txt
	@echo ""
	@echo "=== Тест 4: Получение файла (отдельный файл) ==="
	./$(THREAD_PROG) -get -image test.img -key "secret" -out result1.txt test_file1.txt
	@echo "Содержимое test_file1.txt:"
	@cat result1.txt
	@echo ""
	@echo "=== Тест 5: Получение файла из поддиректории ==="
	./$(THREAD_PROG) -get -image test.img -key "secret" -out result2.txt sub1/sub2/deep2.txt
	@echo "Содержимое deep2.txt:"
	@cat result2.txt
	@echo ""
	@echo "=== Тест 6: Получение файла с глубиной вложенности 4 ==="
	./$(THREAD_PROG) -get -image test.img -key "secret" -out result3.txt sub1/sub2/sub3/sub4/deep4.txt
	@echo "Содержимое deep4.txt:"
	@cat result3.txt
	@echo ""
	@echo "=== Тест 7: Добавление ещё одного файла в существующий образ ==="
	@echo "Новый файл" > test_file3.txt
	./$(THREAD_PROG) -add -key "secret" -image test.img test_file3.txt
	./$(THREAD_PROG) -list -image test.img
	@echo ""
	@echo "=== Тестирование завершено ==="

clean:
	rm -f $(LIB_NAME) $(THREAD_PROG)
	rm -f test.img test_file*.txt result*.txt
	rm -rf test_dir

.PHONY: all test clean
