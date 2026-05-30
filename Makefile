CC = gcc
CFLAGS = -Wall -Wextra -pedantic -fPIC -pthread -D_GNU_SOURCE -O2
LDFLAGS = -ldl -lm -pthread

LIB_NAME = libcaesar.so
THREAD_PROG = thread_file
TEST_PROG = test_loader
SECURE_PROG = secure_copy

.PHONY: all clean

all: $(LIB_NAME) $(THREAD_PROG) $(TEST_PROG) $(SECURE_PROG)

$(LIB_NAME): libcaesar.c
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDFLAGS)

$(THREAD_PROG): thread_file.c $(LIB_NAME)
	$(CC) $(CFLAGS) -o $@ $< -L. -lcaesar $(LDFLAGS) -Wl,-rpath,.

$(TEST_PROG): test_loader.c $(LIB_NAME)
	$(CC) $(CFLAGS) -o $@ $< -L. -lcaesar $(LDFLAGS) -Wl,-rpath,.

$(SECURE_PROG): secure_copy.c $(LIB_NAME)
	$(CC) $(CFLAGS) -o $@ $< -L. -lcaesar $(LDFLAGS) -Wl,-rpath,.

clean:
	rm -f $(LIB_NAME) $(THREAD_PROG) $(TEST_PROG) $(SECURE_PROG) *.o *.container *.enc
	rm -rf test_dir mountpoint log.txt
