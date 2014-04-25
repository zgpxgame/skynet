#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "malloc_hook.h"
#include "jemalloc.h"

#include "skynet.h"

static size_t _used_memory = 0;
static size_t _memory_block = 0;
typedef struct _mem_data {
	uint32_t handle;
	ssize_t allocated;
} mem_data;

#define SLOT_SIZE 0x10000
#define PREFIX_SIZE sizeof(uint32_t)

static mem_data mem_stats[SLOT_SIZE];

static ssize_t*
get_allocated_field(uint32_t handle) {
	int h = (int)(handle & (SLOT_SIZE - 1));
	mem_data *data = &mem_stats[h];
	uint32_t old_handle = data->handle;
	ssize_t old_alloc = data->allocated;
	if(old_handle == 0 || old_alloc <= 0) {
		// data->allocated may less than zero, because it may not count at start.
		if(!__sync_bool_compare_and_swap(&data->handle, old_handle, handle)) {
			return 0;
		}
		if (old_alloc < 0) {
			__sync_bool_compare_and_swap(&data->allocated, old_alloc, 0);
		}
	}
	if(data->handle != handle) {
		return 0;
	}
	return &data->allocated;
}

inline static void 
update_xmalloc_stat_alloc(uint32_t handle, size_t __n) {
	__sync_add_and_fetch(&_used_memory, __n);
	__sync_add_and_fetch(&_memory_block, 1); 
	ssize_t* allocated = get_allocated_field(handle);
	if(allocated) {
		__sync_add_and_fetch(allocated, __n);
	}
}

inline static void
update_xmalloc_stat_free(uint32_t handle, size_t __n) {
	__sync_sub_and_fetch(&_used_memory, __n);
	__sync_sub_and_fetch(&_memory_block, 1);
	ssize_t* allocated = get_allocated_field(handle);
	if(allocated) {
		__sync_sub_and_fetch(allocated, __n);
	}
}

inline static void*
fill_prefix(char* ptr) {
	uint32_t handle = skynet_current_handle();
	size_t size = je_malloc_usable_size(ptr);
	uint32_t *p = (uint32_t *)(ptr + size - sizeof(uint32_t));
	memcpy(p, &handle, sizeof(handle));

	update_xmalloc_stat_alloc(handle, size);
	return ptr;
}

inline static void*
clean_prefix(char* ptr) {
	size_t size = je_malloc_usable_size(ptr);
	uint32_t *p = (uint32_t *)(ptr + size - sizeof(uint32_t));
	uint32_t handle;
	memcpy(&handle, p, sizeof(handle));
	update_xmalloc_stat_free(handle, size);
	return ptr;
}

static void malloc_oom(size_t size) {
	fprintf(stderr, "xmalloc: Out of memory trying to allocate %zu bytes\n",
		size);
	fflush(stderr);
	abort();
}

size_t
malloc_used_memory(void) {
	return _used_memory;
}

size_t
malloc_memory_block(void) {
	return _memory_block;
}

void memory_info_dump(void) {
	je_malloc_stats_print(0,0,0);
}

size_t mallctl_int64(const char* name, size_t* newval) {
	size_t v = 0;
	size_t len = sizeof(v);
	if(newval) {
		je_mallctl(name, &v, &len, newval, sizeof(size_t));
	} else {
		je_mallctl(name, &v, &len, NULL, 0);
	}
	// printf("name: %s, value: %zd\n", name, v);
	return v;
}

int mallctl_opt(const char* name, int* newval) {
	int    v   = 0;
	size_t len = sizeof(v);
	if(newval) {
		int ret = je_mallctl(name, &v, &len, newval, sizeof(int));
		if(ret == 0) {
			printf("set new value(%d) for (%s) succeed\n", *newval, name);
		} else {
			printf("set new value(%d) for (%s) failed: error -> %d\n", *newval, name, ret);
		}
	} else {
		je_mallctl(name, &v, &len, NULL, 0);
	}

	return v;
}

void
dump_c_mem() {
	int i;
	size_t total = 0;
	printf("dump all service mem:\n");
	for(i=0; i<SLOT_SIZE; i++) {
		mem_data* data = &mem_stats[i];
		if(data->handle != 0 && data->allocated != 0) {
			total += data->allocated;
			printf("0x%x -> %zdkb\n", data->handle, data->allocated >> 10);
		}
	}
	printf("+total: %zdkb\n",total >> 10);
}

// hook : malloc, realloc, memalign, free, calloc

void *
skynet_malloc(size_t size) {
	void* ptr = je_malloc(size + PREFIX_SIZE);
	if(!ptr) malloc_oom(size);
	return fill_prefix(ptr);
}

void *
skynet_realloc(void *ptr, size_t size) {
	if (ptr == NULL) return skynet_malloc(size);

	void* rawptr = clean_prefix(ptr);
	void *newptr = je_realloc(rawptr, size+PREFIX_SIZE);
	if(!newptr) malloc_oom(size);
	return fill_prefix(newptr);
}

void
skynet_free(void *ptr) {
	if (ptr == NULL) return;
	void* rawptr = clean_prefix(ptr);
	je_free(rawptr);
}

void *
skynet_calloc(size_t nmemb,size_t size) {
	void* ptr = je_calloc(nmemb + ((PREFIX_SIZE+size-1)/size), size );
	if(!ptr) malloc_oom(size);
	return fill_prefix(ptr);
}

void *
skynet_memalign(size_t alignment, size_t size) {
	void * ptr = NULL;
	je_posix_memalign(&ptr, alignment, size);

	return ptr;
}

char *
skynet_strdup(const char *str) {
	size_t sz = strlen(str);
	char * ret = skynet_malloc(sz+1);
	memcpy(ret, str, sz+1);
	return ret;
}

void * 
skynet_lalloc(void *ud, void *ptr, size_t osize, size_t nsize) {
	if (nsize == 0) {
		skynet_free(ptr);
		return NULL;
	} else {
		return skynet_realloc(ptr, nsize);
	}
}

#ifdef __APPLE__
// register malloc zone

/* zone support for mac osx , from jemalloc/src/zone.c */

#include <mach/mach_error.h>
#include <mach/mach_init.h>
#include <mach/vm_map.h>
#include <malloc/malloc.h>
#include <assert.h>
#include <stdlib.h>

/*
 * The malloc_default_purgeable_zone function is only available on >= 10.6.
 * We need to check whether it is present at runtime, thus the weak_import.
 */
extern malloc_zone_t *malloc_default_purgeable_zone(void) 
__attribute__((weak_import));

/******************************************************************************/
/* Data. */

static malloc_zone_t zone;

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

static size_t	zone_size(malloc_zone_t *zone, void *ptr);
static void	*zone_malloc(malloc_zone_t *zone, size_t size);
static void	*zone_calloc(malloc_zone_t *zone, size_t num, size_t size);
static void	*zone_valloc(malloc_zone_t *zone, size_t size);
static void	zone_free(malloc_zone_t *zone, void *ptr);
static void	*zone_realloc(malloc_zone_t *zone, void *ptr, size_t size);
static void	*zone_memalign(malloc_zone_t *zone, size_t alignment, size_t size);
static void	zone_free_definite_size(malloc_zone_t *zone, void *ptr, size_t size);
static void	*zone_destroy(malloc_zone_t *zone);

/******************************************************************************/
/*
 * Functions.
 */

static size_t
zone_size(malloc_zone_t *zone, void *ptr) {
	if (ptr == NULL)
		return 0;
	return je_malloc_usable_size(ptr) - PREFIX_SIZE;
}

static void *
zone_malloc(malloc_zone_t *zone, size_t size) {
	return (skynet_malloc(size));
}

static void *
zone_calloc(malloc_zone_t *zone, size_t num, size_t size) {
	return (skynet_calloc(num, size));
}

static void *
zone_valloc(malloc_zone_t *zone, size_t size) {
	void* ptr = je_valloc(size + PREFIX_SIZE);
	if(!ptr) malloc_oom(size);
	return fill_prefix(ptr);
}

static void
zone_free(malloc_zone_t *zone, void *ptr) {
	skynet_free(ptr);
}

static void *
zone_realloc(malloc_zone_t *zone, void *ptr, size_t size) {
	return (skynet_realloc(ptr, size));
}

static void *
zone_memalign(malloc_zone_t *zone, size_t alignment, size_t size) {
	return skynet_memalign(alignment, size);
}

static void
zone_free_definite_size(malloc_zone_t *zone, void *ptr, size_t size) {
	skynet_free(ptr);
}

static void *
zone_destroy(malloc_zone_t *zone) {
	/* This function should never be called. */
	return (NULL);
}

void
malloc_inithook(void) {
	malloc_zone_t *default_zone = malloc_default_zone();
	if (!default_zone->zone_name ||
		strcmp(default_zone->zone_name, "DefaultMallocZone") != 0) {
		printf("The %s should be DefaultMallocZone", default_zone->zone_name);
		exit(1);
		return;
	}
	printf("register %s\n", default_zone->zone_name);

	zone.size = (void *)zone_size;
	zone.malloc = (void *)zone_malloc;
	zone.calloc = (void *)zone_calloc;
	zone.valloc = (void *)zone_valloc;
	zone.free = (void *)zone_free;
	zone.realloc = (void *)zone_realloc;
	zone.destroy = (void *)zone_destroy;
	zone.zone_name = "skynet_zone";
	zone.batch_malloc = NULL;
	zone.batch_free = NULL;
	zone.introspect = NULL;
	zone.version = 0;
	zone.memalign = zone_memalign;
	zone.free_definite_size = zone_free_definite_size;
	zone.pressure_relief = NULL;

	/*
	 * The default purgeable zone is created lazily by OSX's libc.  It uses
	 * the default zone when it is created for "small" allocations
	 * (< 15 KiB), but assumes the default zone is a scalable_zone.  This
	 * obviously fails when the default zone is the jemalloc zone, so
	 * malloc_default_purgeable_zone is called beforehand so that the
	 * default purgeable zone is created when the default zone is still
	 * a scalable_zone.  As purgeable zones only exist on >= 10.6, we need
	 * to check for the existence of malloc_default_purgeable_zone() at
	 * run time.
	 */
	if (malloc_default_purgeable_zone != NULL)
		malloc_default_purgeable_zone();

	/* Register the custom zone.  At this point it won't be the default. */
	malloc_zone_register(&zone);

	/*
	 * Unregister and reregister the default zone.  On OSX >= 10.6,
	 * unregistering takes the last registered zone and places it at the
	 * location of the specified zone.  Unregistering the default zone thus
	 * makes the last registered one the default.  On OSX < 10.6,
	 * unregistering shifts all registered zones.  The first registered zone
	 * then becomes the default.
	 */
	do {
		default_zone = malloc_default_zone();
		malloc_zone_unregister(default_zone);
		malloc_zone_register(default_zone);
	} while (malloc_default_zone() != &zone);
}

#else

void 
malloc_inithook(void) {
}

#endif
