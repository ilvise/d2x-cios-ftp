/*
 * Host test stub of cios-lib/types.h: the fixed size types used by
 * net.c, mapped to <stdint.h>. See host_shim.c.
 */
#ifndef _TYPES_H_
#define _TYPES_H_

#include <stdint.h>
#include <stddef.h>

typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define ATTRIBUTE_ALIGN(v) __attribute__((aligned(v)))

#endif
