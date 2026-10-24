#pragma once
#include <stddef.h>
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define SIZEOF_FIELD(t,f) sizeof(((t*)0)->f)
#define ARG_UNUSED(x) (void)(x)
#define __fallthrough __attribute__((fallthrough))
#define ROUND_UP(x,a) (((x)+(a)-1)&~((a)-1))
