#include "libcaesar.h"
#include <string.h>

static char caesar_key = 0;

void set_key(char key) {
    caesar_key = key;
}

void caesar(void* src, void* dst, int len) {
    unsigned char* s = (unsigned char*)src;
    unsigned char* d = (unsigned char*)dst;
    
    for (int i = 0; i < len; i++) {
        d[i] = s[i] + caesar_key;
    }
}
