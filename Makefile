CC = gcc
CFLAGS = -Wall -Wextra -pedantic -fPIC
LIB_NAME = libcaesar.so
TEST_PROG = test_loader

all: $(LIB_NAME)

$(LIB_NAME): libcaesar.c
	 $(CC) $(CFLAGS) -shared -o $(LIB_NAME) libcaesar.c

install: $(LIB_NAME)
	 sudo cp $(LIB_NAME) /usr/local/lib/
	 sudo ldconfig

test: $(LIB_NAME) test_loader.c
	$(CC) -o $(TEST_PROG) test_loader.c -ldl
	 # Создаем тестовый файл если нет
	 echo "Hello, student MIREA" > input.txt
	 ./$(TEST_PROG) ./$(LIB_NAME) 65 input.txt output.txt
	 @echo "--- Результат шифрования (output.txt) ---"
	 xxd output.txt
	 @echo "--- Расшифровка обратно ---"
	 ./$(TEST_PROG) ./$(LIB_NAME) 65 output.txt decrypted.txt
	 cat decrypted.txt

clean:
	 rm -f $(LIB_NAME) $(TEST_PROG) input.txt output.txt decrypted.txt
