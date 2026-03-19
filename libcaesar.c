#include "libcaesar.h"
static char global_key = 0;

void set_key(char key) {
    global_key = key;
}

void caesar(void* src, void* dst, int len) {
    if (!src || !dst || len <= 0) return;
    
    unsigned char* s = (unsigned char*)src;
    unsigned char* d = (unsigned char*)dst;

    for (int i = 0; i < len; i++) {
        d[i] = s[i] ^ (unsigned char)global_key;
    }
}
