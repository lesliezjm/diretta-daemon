// glibc compatibility shim for SDK compiled with glibc 2.38+
// Provides __isoc23_strtol on older glibc only. On glibc 2.38 and newer,
// <stdlib.h> redirects strtol() to __isoc23_strtol(); defining the shim there
// would make it call itself forever.
#define _GNU_SOURCE
#include <features.h>
#include <stdlib.h>

#if !defined(__GLIBC_PREREQ) || !__GLIBC_PREREQ(2, 38)
long __isoc23_strtol(const char *nptr, char **endptr, int base) {
    return strtol(nptr, endptr, base);
}
#endif
