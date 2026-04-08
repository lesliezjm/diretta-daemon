// glibc compatibility shim for SDK compiled with glibc 2.38+
// Provides __isoc23_strtol as aliases to strtol on older glibc
#define _GNU_SOURCE
#include <stdlib.h>

long __isoc23_strtol(const char *nptr, char **endptr, int base) {
    return strtol(nptr, endptr, base);
}
