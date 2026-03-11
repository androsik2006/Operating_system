CC = gcc
CFLAGS = -Wall -pthread -g
LDFLAGS = -pthread

# Targets
all: libcaesar.so secure_copy

# Build shared library
libcaesar.so: libcaesar.c libcaesar.h
	$(CC) $(CFLAGS) -fPIC -shared -o libcaesar.so libcaesar.c
…install: libcaesar.so
	sudo cp libcaesar.so /usr/local/lib/
	sudo ldconfig

.PHONY: all clean install
