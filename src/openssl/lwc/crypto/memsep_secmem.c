/*
 * Copyright 2015-2017 The OpenSSL Project Authors. All Rights Reserved.
 * Copyright 2004-2014, Akamai Technologies. All Rights Reserved.
 *
 * Licensed under the OpenSSL license (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

/*
 * This file is in two halves. The first half implements the public API
 * to be used by external consumers, and to be used by OpenSSL to store
 * data in a "secure arena." The second half implements the secure arena.
 * For details on that implementation, see below (look for uppercase
 * "SECURE HEAP IMPLEMENTATION").
 */
#include "e_os.h"
#include <openssl/crypto.h>

#include <string.h>

#define IMPLEMENTED

#if defined(OPENSSL_SYS_LINUX) || defined(OPENSSL_SYS_UNIX)
# define IMPLEMENTED
# include <stdlib.h>
# include <assert.h>
# include <unistd.h>
# include <sys/types.h>
# include <sys/mman.h>
# if defined(OPENSSL_SYS_LINUX)
#  include <sys/syscall.h>
#  include <linux/mman.h>
#  include <errno.h>
# endif
# include <sys/param.h>
# include <sys/stat.h>
# include <fcntl.h>
#endif

#define CLEAR(p, s) OPENSSL_cleanse(p, s)
#ifndef PAGE_SIZE
# define PAGE_SIZE    4096
#endif

#ifdef IMPLEMENTED
static size_t memsep_secure_mem_used;

static int memsep_secure_mem_initialized;

static CRYPTO_RWLOCK *memsep_sec_malloc_lock = NULL;

/*
 * These are the functions that must be implemented by a secure heap (sh).
 */
static int sh_init(size_t size, int minsize);
static void *sh_malloc(size_t size);
static void sh_free(void *ptr);
static void sh_done(void);
static size_t sh_actual_size(char *ptr);
static int sh_allocated(const char *ptr);
#endif

int MEMSEP_secure_malloc_init(size_t size, int minsize)
{
#ifdef IMPLEMENTED
    int ret = 0;

    if (!memsep_secure_mem_initialized) {
        memsep_sec_malloc_lock = CRYPTO_THREAD_glock_new("sec_malloc");
        if (memsep_sec_malloc_lock == NULL)
            return 0;
        if ((ret = sh_init(size, minsize)) != 0) {
            memsep_secure_mem_initialized = 1;
        } else {
            CRYPTO_THREAD_lock_free(memsep_sec_malloc_lock);
            memsep_sec_malloc_lock = NULL;
        }
    }

    return ret;
#else
    return 0;
#endif /* IMPLEMENTED */
}

int MEMSEP_secure_malloc_done()
{
#ifdef IMPLEMENTED
    if (memsep_secure_mem_used == 0) {
        sh_done();
        memsep_secure_mem_initialized = 0;
        CRYPTO_THREAD_lock_free(memsep_sec_malloc_lock);
        memsep_sec_malloc_lock = NULL;
        return 1;
    }
#endif /* IMPLEMENTED */
    return 0;
}

int MEMSEP_secure_malloc_initialized()
{
#ifdef IMPLEMENTED
    return memsep_secure_mem_initialized;
#else
    return 0;
#endif /* IMPLEMENTED */
}

void *MEMSEP_secure_malloc(size_t num, const char *file, int line)
{
#ifdef IMPLEMENTED
    void *ret;
    size_t actual_size;

    if (!memsep_secure_mem_initialized) {
        return CRYPTO_malloc(num, file, line);
    }
    CRYPTO_THREAD_write_lock(memsep_sec_malloc_lock);
    ret = sh_malloc(num);
    actual_size = ret ? sh_actual_size(ret) : 0;
    memsep_secure_mem_used += actual_size;
    CRYPTO_THREAD_unlock(memsep_sec_malloc_lock);
    //    fprintf(stderr, "%s:%d %lld total %lld\n", file, line, num, memsep_secure_mem_used);
    return ret;
#else
    return CRYPTO_malloc(num, file, line);
#endif /* IMPLEMENTED */
}

void *MEMSEP_secure_zalloc(size_t num, const char *file, int line)
{
    void *ret = MEMSEP_secure_malloc(num, file, line);

    if (ret != NULL)
        memset(ret, 0, num);
    return ret;
}

void MEMSEP_secure_free(void *ptr, const char *file, int line)
{
#ifdef IMPLEMENTED
    size_t actual_size;

    if (ptr == NULL)
        return;
    if (!MEMSEP_secure_allocated(ptr)) {
        CRYPTO_free(ptr, file, line);
        return;
    }
    CRYPTO_THREAD_write_lock(memsep_sec_malloc_lock);
    actual_size = sh_actual_size(ptr);
    CLEAR(ptr, actual_size);
    memsep_secure_mem_used -= actual_size;
    sh_free(ptr);
    CRYPTO_THREAD_unlock(memsep_sec_malloc_lock);
#else
    CRYPTO_free(ptr, file, line);
#endif /* IMPLEMENTED */
}

void MEMSEP_secure_clear_free(void *ptr, size_t num,
                              const char *file, int line)
{
#ifdef IMPLEMENTED
    size_t actual_size;

    if (ptr == NULL)
        return;
    if (!MEMSEP_secure_allocated(ptr)) {
        OPENSSL_cleanse(ptr, num);
        CRYPTO_free(ptr, file, line);
        return;
    }
    CRYPTO_THREAD_write_lock(memsep_sec_malloc_lock);
    actual_size = sh_actual_size(ptr);
    CLEAR(ptr, actual_size);
    memsep_secure_mem_used -= actual_size;
    sh_free(ptr);
    CRYPTO_THREAD_unlock(memsep_sec_malloc_lock);
#else
    if (ptr == NULL)
        return;
    OPENSSL_cleanse(ptr, num);
    CRYPTO_free(ptr, file, line);
#endif /* IMPLEMENTED */
}

int MEMSEP_secure_allocated(const void *ptr)
{
#ifdef IMPLEMENTED
    int ret;

    if (!memsep_secure_mem_initialized)
        return 0;
    CRYPTO_THREAD_write_lock(memsep_sec_malloc_lock);
    ret = sh_allocated(ptr);
    CRYPTO_THREAD_unlock(memsep_sec_malloc_lock);
    return ret;
#else
    return 0;
#endif /* IMPLEMENTED */
}

size_t MEMSEP_secure_used()
{
#ifdef IMPLEMENTED
    return memsep_secure_mem_used;
#else
    return 0;
#endif /* IMPLEMENTED */
}

size_t MEMSEP_secure_actual_size(void *ptr)
{
#ifdef IMPLEMENTED
    size_t actual_size;

    CRYPTO_THREAD_write_lock(memsep_sec_malloc_lock);
    actual_size = sh_actual_size(ptr);
    CRYPTO_THREAD_unlock(memsep_sec_malloc_lock);
    return actual_size;
#else
    return 0;
#endif
}
/* END OF PAGE ...

   ... START OF PAGE */

/*
 * SECURE HEAP IMPLEMENTATION
 */
#ifdef IMPLEMENTED


/*
 * The implementation provided here uses a fixed-sized mmap() heap,
 * which is locked into memory, not written to core files, and protected
 * on either side by an unmapped page, which will catch pointer overruns
 * (or underruns) and an attempt to read data out of the secure heap.
 * Free'd memory is zero'd or otherwise cleansed.
 *
 * This is a pretty standard buddy allocator.  We keep areas in a multiple
 * of "sh.minsize" units.  The freelist and bitmaps are kept separately,
 * so all (and only) data is kept in the mmap'd heap.
 *
 * This code assumes eight-bit bytes.  The numbers 3 and 7 are all over the
 * place.
 */

#define ONE ((size_t)1)

# define TESTBIT(t, b)  (t[(b) >> 3] &  (ONE << ((b) & 7)))
# define SETBIT(t, b)   (t[(b) >> 3] |= (ONE << ((b) & 7)))
# define CLEARBIT(t, b) (t[(b) >> 3] &= (0xFF & ~(ONE << ((b) & 7))))

#define WITHIN_ARENA(p) \
    ((char*)(p) >= memsep_sh.arena && (char*)(p) < &memsep_sh.arena[memsep_sh.arena_size])
#define WITHIN_FREELIST(p) \
    ((char*)(p) >= (char*)memsep_sh.freelist && (char*)(p) < (char*)&memsep_sh.freelist[memsep_sh.freelist_size])


typedef struct sh_list_st
{
    struct sh_list_st *next;
    struct sh_list_st **p_next;
} SH_LIST;

typedef struct sh_st
{
    char* map_result;
    size_t map_size;
    char *arena;
    size_t arena_size;
    char **freelist;
    ossl_ssize_t freelist_size;
    size_t minsize;
    unsigned char *bittable;
    unsigned char *bitmalloc;
    size_t bittable_size; /* size in bits */
} SH;

static SH memsep_sh;

static size_t sh_getlist(char *ptr)
{
    ossl_ssize_t list = memsep_sh.freelist_size - 1;
    size_t bit = (memsep_sh.arena_size + ptr - memsep_sh.arena) / memsep_sh.minsize;

    for (; bit; bit >>= 1, list--) {
        if (TESTBIT(memsep_sh.bittable, bit))
            break;
        OPENSSL_assert((bit & 1) == 0);
    }

    return list;
}


static int sh_testbit(char *ptr, int list, unsigned char *table)
{
    size_t bit;

    OPENSSL_assert(list >= 0 && list < memsep_sh.freelist_size);
    OPENSSL_assert(((ptr - memsep_sh.arena) & ((memsep_sh.arena_size >> list) - 1)) == 0);
    bit = (ONE << list) + ((ptr - memsep_sh.arena) / (memsep_sh.arena_size >> list));
    OPENSSL_assert(bit > 0 && bit < memsep_sh.bittable_size);
    return TESTBIT(table, bit);
}

static void sh_clearbit(char *ptr, int list, unsigned char *table)
{
    size_t bit;

    OPENSSL_assert(list >= 0 && list < memsep_sh.freelist_size);
    OPENSSL_assert(((ptr - memsep_sh.arena) & ((memsep_sh.arena_size >> list) - 1)) == 0);
    bit = (ONE << list) + ((ptr - memsep_sh.arena) / (memsep_sh.arena_size >> list));
    OPENSSL_assert(bit > 0 && bit < memsep_sh.bittable_size);
    OPENSSL_assert(TESTBIT(table, bit));
    CLEARBIT(table, bit);
}

static void sh_setbit(char *ptr, int list, unsigned char *table)
{
    size_t bit;

    OPENSSL_assert(list >= 0 && list < memsep_sh.freelist_size);
    OPENSSL_assert(((ptr - memsep_sh.arena) & ((memsep_sh.arena_size >> list) - 1)) == 0);
    bit = (ONE << list) + ((ptr - memsep_sh.arena) / (memsep_sh.arena_size >> list));
    OPENSSL_assert(bit > 0 && bit < memsep_sh.bittable_size);
    OPENSSL_assert(!TESTBIT(table, bit));
    SETBIT(table, bit);
}

static void sh_add_to_list(char **list, char *ptr)
{
    SH_LIST *temp;

    OPENSSL_assert(WITHIN_FREELIST(list));
    OPENSSL_assert(WITHIN_ARENA(ptr));

    temp = (SH_LIST *)ptr;
    temp->next = *(SH_LIST **)list;
    OPENSSL_assert(temp->next == NULL || WITHIN_ARENA(temp->next));
    temp->p_next = (SH_LIST **)list;

    if (temp->next != NULL) {
        OPENSSL_assert((char **)temp->next->p_next == list);
        temp->next->p_next = &(temp->next);
    }

    *list = ptr;
}

static void sh_remove_from_list(char *ptr)
{
    SH_LIST *temp, *temp2;

    temp = (SH_LIST *)ptr;
    if (temp->next != NULL)
        temp->next->p_next = temp->p_next;
    *temp->p_next = temp->next;
    if (temp->next == NULL)
        return;

    temp2 = temp->next;
    OPENSSL_assert(WITHIN_FREELIST(temp2->p_next) || WITHIN_ARENA(temp2->p_next));
}


static int sh_init(size_t size, int minsize)
{
    int ret;
    size_t i;
    size_t pgsize;
    size_t aligned;

    memset(&memsep_sh, 0, sizeof memsep_sh);

    /* make sure size and minsize are powers of 2 */
    OPENSSL_assert(size > 0);
    OPENSSL_assert((size & (size - 1)) == 0);
    OPENSSL_assert(minsize > 0);
    OPENSSL_assert((minsize & (minsize - 1)) == 0);
    if (size <= 0 || (size & (size - 1)) != 0)
        goto err;
    if (minsize <= 0 || (minsize & (minsize - 1)) != 0)
        goto err;

    while (minsize < (int)sizeof(SH_LIST))
        minsize *= 2;

    memsep_sh.arena_size = size;
    memsep_sh.minsize = minsize;
    memsep_sh.bittable_size = (memsep_sh.arena_size / memsep_sh.minsize) * 2;

    /* Prevent allocations of size 0 later on */
    if (memsep_sh.bittable_size >> 3 == 0)
        goto err;

    memsep_sh.freelist_size = -1;
    for (i = memsep_sh.bittable_size; i; i >>= 1)
        memsep_sh.freelist_size++;

    memsep_sh.freelist = OPENSSL_zalloc(memsep_sh.freelist_size * sizeof (char *));
    OPENSSL_assert(memsep_sh.freelist != NULL);
    if (memsep_sh.freelist == NULL)
        goto err;

    memsep_sh.bittable = OPENSSL_zalloc(memsep_sh.bittable_size >> 3);
    OPENSSL_assert(memsep_sh.bittable != NULL);
    if (memsep_sh.bittable == NULL)
        goto err;

    memsep_sh.bitmalloc = OPENSSL_zalloc(memsep_sh.bittable_size >> 3);
    OPENSSL_assert(memsep_sh.bitmalloc != NULL);
    if (memsep_sh.bitmalloc == NULL)
        goto err;

    /* Allocate space for heap, and two extra pages as guards */
#if defined(_SC_PAGE_SIZE) || defined (_SC_PAGESIZE)
    {
# if defined(_SC_PAGE_SIZE)
        long tmppgsize = sysconf(_SC_PAGE_SIZE);
# else
        long tmppgsize = sysconf(_SC_PAGESIZE);
# endif
        if (tmppgsize < 1)
            pgsize = PAGE_SIZE;
        else
            pgsize = (size_t)tmppgsize;
    }
#else
    pgsize = PAGE_SIZE;
#endif
    memsep_sh.map_size = pgsize + memsep_sh.arena_size + pgsize;
    if (1) {
#ifdef MAP_ANON
        memsep_sh.map_result = mmap(NULL, memsep_sh.map_size,
                             PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
    } else {
#endif
        int fd;

        memsep_sh.map_result = MAP_FAILED;
        if ((fd = open("/dev/zero", O_RDWR)) >= 0) {
            memsep_sh.map_result = mmap(NULL, memsep_sh.map_size,
                                 PROT_READ|PROT_WRITE, MAP_PRIVATE, fd, 0);
            close(fd);
        }
    }
    if (memsep_sh.map_result == MAP_FAILED)
        goto err;
    memsep_sh.arena = (char *)(memsep_sh.map_result + pgsize);
    sh_setbit(memsep_sh.arena, 0, memsep_sh.bittable);
    sh_add_to_list(&memsep_sh.freelist[0], memsep_sh.arena);

    /* Now try to add guard pages and lock into memory. */
    ret = 1;

    /* Starting guard is already aligned from mmap. */
    if (mprotect(memsep_sh.map_result, pgsize, PROT_NONE) < 0)
        ret = 2;

    /* Ending guard page - need to round up to page boundary */
    aligned = (pgsize + memsep_sh.arena_size + (pgsize - 1)) & ~(pgsize - 1);
    if (mprotect(memsep_sh.map_result + aligned, pgsize, PROT_NONE) < 0)
        ret = 2;

#if defined(OPENSSL_SYS_LINUX) && defined(MLOCK_ONFAULT) && defined(SYS_mlock2)
    if (syscall(SYS_mlock2, memsep_sh.arena, memsep_sh.arena_size, MLOCK_ONFAULT) < 0) {
        if (errno == ENOSYS) {
            if (mlock(memsep_sh.arena, memsep_sh.arena_size) < 0)
                ret = 2;
        } else {
            ret = 2;
        }
    }
#else
    if (mlock(memsep_sh.arena, memsep_sh.arena_size) < 0)
        ret = 2;
#endif
#ifdef MADV_DONTDUMP
    if (madvise(memsep_sh.arena, memsep_sh.arena_size, MADV_DONTDUMP) < 0)
        ret = 2;
#endif

    return ret;

 err:
    sh_done();
    return 0;
}

static void sh_done()
{
    OPENSSL_free(memsep_sh.freelist);
    OPENSSL_free(memsep_sh.bittable);
    OPENSSL_free(memsep_sh.bitmalloc);
    if (memsep_sh.map_result != NULL && memsep_sh.map_size)
        munmap(memsep_sh.map_result, memsep_sh.map_size);
    memset(&memsep_sh, 0, sizeof memsep_sh);
}

static int sh_allocated(const char *ptr)
{
    return WITHIN_ARENA(ptr) ? 1 : 0;
}

static char *sh_find_my_buddy(char *ptr, int list)
{
    size_t bit;
    char *chunk = NULL;

    bit = (ONE << list) + (ptr - memsep_sh.arena) / (memsep_sh.arena_size >> list);
    bit ^= 1;

    if (TESTBIT(memsep_sh.bittable, bit) && !TESTBIT(memsep_sh.bitmalloc, bit))
        chunk = memsep_sh.arena + ((bit & ((ONE << list) - 1)) * (memsep_sh.arena_size >> list));

    return chunk;
}

static void *sh_malloc(size_t size)
{
    ossl_ssize_t list, slist;
    size_t i;
    char *chunk;

    if (size > memsep_sh.arena_size)
        return NULL;

    list = memsep_sh.freelist_size - 1;
    for (i = memsep_sh.minsize; i < size; i <<= 1)
        list--;
    if (list < 0)
        return NULL;

    /* try to find a larger entry to split */
    for (slist = list; slist >= 0; slist--)
        if (memsep_sh.freelist[slist] != NULL)
            break;
    if (slist < 0)
        return NULL;

    /* split larger entry */
    while (slist != list) {
        char *temp = memsep_sh.freelist[slist];

        /* remove from bigger list */
        OPENSSL_assert(!sh_testbit(temp, slist, memsep_sh.bitmalloc));
        sh_clearbit(temp, slist, memsep_sh.bittable);
        sh_remove_from_list(temp);
        OPENSSL_assert(temp != memsep_sh.freelist[slist]);

        /* done with bigger list */
        slist++;

        /* add to smaller list */
        OPENSSL_assert(!sh_testbit(temp, slist, memsep_sh.bitmalloc));
        sh_setbit(temp, slist, memsep_sh.bittable);
        sh_add_to_list(&memsep_sh.freelist[slist], temp);
        OPENSSL_assert(memsep_sh.freelist[slist] == temp);

        /* split in 2 */
        temp += memsep_sh.arena_size >> slist;
        OPENSSL_assert(!sh_testbit(temp, slist, memsep_sh.bitmalloc));
        sh_setbit(temp, slist, memsep_sh.bittable);
        sh_add_to_list(&memsep_sh.freelist[slist], temp);
        OPENSSL_assert(memsep_sh.freelist[slist] == temp);

        OPENSSL_assert(temp-(memsep_sh.arena_size >> slist) == sh_find_my_buddy(temp, slist));
    }

    /* peel off memory to hand back */
    chunk = memsep_sh.freelist[list];
    OPENSSL_assert(sh_testbit(chunk, list, memsep_sh.bittable));
    sh_setbit(chunk, list, memsep_sh.bitmalloc);
    sh_remove_from_list(chunk);

    OPENSSL_assert(WITHIN_ARENA(chunk));

    return chunk;
}

static void sh_free(void *ptr)
{
    size_t list;
    void *buddy;

    if (ptr == NULL)
        return;
    OPENSSL_assert(WITHIN_ARENA(ptr));
    if (!WITHIN_ARENA(ptr))
        return;

    list = sh_getlist(ptr);
    OPENSSL_assert(sh_testbit(ptr, list, memsep_sh.bittable));
    sh_clearbit(ptr, list, memsep_sh.bitmalloc);
    sh_add_to_list(&memsep_sh.freelist[list], ptr);

    /* Try to coalesce two adjacent free areas. */
    while ((buddy = sh_find_my_buddy(ptr, list)) != NULL) {
        OPENSSL_assert(ptr == sh_find_my_buddy(buddy, list));
        OPENSSL_assert(ptr != NULL);
        OPENSSL_assert(!sh_testbit(ptr, list, memsep_sh.bitmalloc));
        sh_clearbit(ptr, list, memsep_sh.bittable);
        sh_remove_from_list(ptr);
        OPENSSL_assert(!sh_testbit(ptr, list, memsep_sh.bitmalloc));
        sh_clearbit(buddy, list, memsep_sh.bittable);
        sh_remove_from_list(buddy);

        list--;

        if (ptr > buddy)
            ptr = buddy;

        OPENSSL_assert(!sh_testbit(ptr, list, memsep_sh.bitmalloc));
        sh_setbit(ptr, list, memsep_sh.bittable);
        sh_add_to_list(&memsep_sh.freelist[list], ptr);
        OPENSSL_assert(memsep_sh.freelist[list] == ptr);
    }
}

static size_t sh_actual_size(char *ptr)
{
    int list;

    OPENSSL_assert(WITHIN_ARENA(ptr));
    if (!WITHIN_ARENA(ptr))
        return 0;
    list = sh_getlist(ptr);
    OPENSSL_assert(sh_testbit(ptr, list, memsep_sh.bittable));
    return memsep_sh.arena_size / (ONE << list);
}
#endif /* IMPLEMENTED */

#undef IMPLEMENTED
