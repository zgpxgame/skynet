#ifndef __MALLOC_HOOK_H
#define __MALLOC_HOOK_H

#include <stdlib.h>

size_t malloc_used_memory(void);
size_t malloc_memory_block(void);
void   memory_info_dump(void);
size_t mallctl_int64(const char* name, size_t* newval);
int    mallctl_opt(const char* name, int* newval);
void   dump_c_mem(void);

void malloc_inithook(void);

#endif /* __MALLOC_HOOK_H */

