/* Windows (mingw) без <endian.h>: все поддерживаемые цели little-endian. */
#ifndef SANOTTS_WIN_ENDIAN_H
#define SANOTTS_WIN_ENDIAN_H
#define LITTLE_ENDIAN 1234
#define BIG_ENDIAN 4321
#define BYTE_ORDER LITTLE_ENDIAN
#define __LITTLE_ENDIAN LITTLE_ENDIAN
#define __BIG_ENDIAN BIG_ENDIAN
#define __BYTE_ORDER BYTE_ORDER
#define le16toh(x) (x)
#define le32toh(x) (x)
#define le64toh(x) (x)
#define htole16(x) (x)
#define htole32(x) (x)
#define htole64(x) (x)
#endif
