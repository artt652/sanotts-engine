/* Подключается ко всем файлам сборки (-include). На Linux/Android пуст;
 * для Windows (mingw) — недостающие POSIX-функции, которые зовёт espeak-ng. */
#ifndef SANOTTS_COMPAT_H
#define SANOTTS_COMPAT_H
#ifdef _WIN32
#include <string.h>
#include <errno.h>
static __inline int sanotts_strerror_r(int e, char *b, size_t n) { return strerror_s(b, n, e); }
#define strerror_r(e, b, n) sanotts_strerror_r((e), (b), (n))
#endif
#endif
