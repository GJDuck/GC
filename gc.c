/*
 * gc.c
 * Copyright (C) 2013
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * This is a very simple conservative GC implementation for single-threaded
 * x86_64/AMD64.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "gc.h"

/*
 * GC tuning
 */
#define GC_SPACE_FACTOR         1.75            // GC growth factor.
#define GC_MIN_TRIGGER          100000
#define GC_FREELIST_LEN         256
#define GC_PROTECT_LEN          16
#define GC_MARK_STACK_SIZE      0x40000000      // 1 GB
#define GC_RETURN_SWEEP         8
#define GC_MAX_ROOT_SIZE        0x40000000      // 1 GB
#define GC_MAX_MARK_PUSH        1024
#define GC_PAGESIZE             4096

/*
 * A GC free-list node.
 */
struct gc_freelist_s
{
    // NOTE: the 'next' is stored as a hidden pointer (see gc_hide_pointer).
    //       This is to prevent marking ever following the freelist.
    struct gc_freelist_s *next;                 // Next free chunk
};

/*
 * Mark stack node.
 */
struct gc_markstack_s
{
    void **startptr;                            // Start pointer.
    void **endptr;                              // End pointer.
};
typedef struct gc_markstack_s *gc_markstack_t;
typedef uint64_t gc_markunit_t;

/*
 * Root node.
 */
struct gc_root_s
{
    void *ptr;                                  // Pointer (static)
    size_t size;                                // Size (static)
    void **ptrptr;                              // Start pointer.
    size_t *sizeptr;                            // Size pointer.
    size_t elemsize;                            // Element size.
    struct gc_root_s *next;                     // Next pointer.
};
typedef struct gc_root_s *gc_root_t;

/*
 * GC globals.
 */

// General:
static bool gc_inited = false;                  // Is GC initialised?
static bool gc_enabled = true;                  // Is collection enabled?
static void *gc_stackbottom;                    // Stack bottom.
struct gc_region_s __gc_regions[GC_NUM_REGIONS] = {{0}};
static void *gc_markstack;                      // Mark-stack.
static gc_root_t gc_roots = NULL;               // All GC roots.
static gc_error_func_t gc_error_func = NULL;    // Memory error callback.

// Timing and stats related:
static ssize_t gc_total_size = 0;               // Total size.
static ssize_t gc_alloc_size = 0;               // Total allocation (since GC).
static ssize_t gc_trigger_size = GC_MIN_TRIGGER;// GC trigger size.
static ssize_t gc_used_size  = 0;               // Total used memory.

/*
 * GC debugging.
 */
#ifndef NODEBUG
#include <stdarg.h>
static void gc_debug(const char *format, ...)
{
    fprintf(stderr, "GC: ");
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    if (format[0] == '\0' || format[strlen(format)-1] != '\n')
        fputc('\n', stderr);
}
#else       /* NODEBUG */
#define gc_debug(...)
#endif      /* NODEBUG */

/*
 * GC Prototypes.
 */
static void __attribute__((noinline)) *gc_stacktop(void);
static void gc_add_root(gc_root_t root);
static void gc_mark_init(void);
static void gc_mark(gc_root_t roots);
static void gc_sweep(void);
static inline bool gc_is_marked_index(uint8_t *markptr_0, uint32_t idx);

#define gc_read_prefetch(ptr)   __builtin_prefetch((ptr), 0, 1)
#define gc_write_prefetch(ptr)  __builtin_prefetch((ptr), 1)

/*
 * GC platform specific.
 */
#ifdef __MINGW32__

/*
 * Windows.
 */

#include <windows.h>
#include <winnt.h>

static void *gc_get_memory(void)
{
    // Note: Windows (stupidly) assumes that if we are reserving address
    //       space, then there must be enough physical memory to fill that
    //       region.  The work-around is to do lots of small allocates that
    //       do not violate this assumption.
    size_t increment = 256 * 1048576;   // 256Mb
    for (size_t i = 0; i < GC_REGION_SIZE*GC_NUM_REGIONS; i += increment)
    {
        void *addr;
        if ((addr = VirtualAlloc(GC_MEMORY + i, increment, MEM_RESERVE,
                PAGE_READWRITE)) != GC_MEMORY + i)
            return NULL;
    }
    return GC_MEMORY;
}
static void *gc_get_mark_memory(size_t size)
{
    return VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_READWRITE);
}
static void gc_free_memory(void *ptr, size_t size)
{
    VirtualFree(ptr, 0, MEM_RELEASE);
}
static void gc_zero_memory(void *ptr, size_t size)
{
    memset(ptr, 0, size + GC_PAGESIZE);
}
static int gc_protect_memory(void *ptr, size_t size)
{
    void *ptr1 = (void *)(((uintptr_t)ptr / GC_PAGESIZE) * GC_PAGESIZE);
    void *result = VirtualAlloc(ptr1, size + (ptr-ptr1), MEM_COMMIT,
        PAGE_READWRITE);
    return (ptr1 != result);
}
struct _TEB
{
    NT_TIB NtTib;
};
static void *gc_get_stackbottom(void)
{
    return NtCurrentTeb()->NtTib.StackBase;   
}
#else       /* __MINGW32__ */

/*
 * Linux/MACOSX.
 */

#include <sys/mman.h>
#include <sys/syscall.h>

static void *gc_get_memory(void)
{
#ifdef __APPLE__
    int flags = MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_FIXED;
#else       /* __APPLE__ */
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED;
#endif      /* __APPLE__ */
    void *ptr = mmap(GC_MEMORY, GC_REGION_SIZE*GC_NUM_REGIONS, 
        PROT_READ | PROT_WRITE, flags, -1, 0);
    return (ptr == MAP_FAILED? NULL: ptr);
}
static void *gc_get_mark_memory(size_t size)
{
#ifdef __APPLE__
    int flags = MAP_PRIVATE | MAP_ANON | MAP_NORESERVE;
#else       /* __APPLE__ */
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
#endif      /* __APPLE__ */
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, flags, -1, 0);
    return (ptr == MAP_FAILED? NULL: ptr);
}
static void gc_zero_memory(void *ptr, size_t size)
{
    size += GC_PAGESIZE;
#ifdef __APPLE__
    memset(ptr, 0, size);
#else       /* __APPLE__ */
    madvise(ptr, size, MADV_DONTNEED);
#endif      /* __APPLE__ */
}
static void gc_free_memory(void *ptr, size_t size)
{
    munmap(ptr, size);
}
static int gc_protect_memory(void *ptr, size_t size)
{
    void *ptr1 = (void *)(((uintptr_t)ptr / GC_PAGESIZE) * GC_PAGESIZE);
    return mprotect(ptr1, size + (ptr-ptr1), PROT_READ | PROT_WRITE);
}
static void *gc_get_stackbottom(void)
{
    void *stackbottom;
    stackbottom = (void *)gc_stacktop();
    stackbottom = (void *)(((uintptr_t)(stackbottom + GC_PAGESIZE)
        / GC_PAGESIZE) * GC_PAGESIZE);
    unsigned char vec;
#ifdef __APPLE__
    while (mincore(stackbottom, GC_PAGESIZE, &vec) == 0 && vec != 0)
        stackbottom += GC_PAGESIZE;
#else       /* __APPLE__ */
    while (mincore(stackbottom, GC_PAGESIZE, &vec) == 0)
        stackbottom += GC_PAGESIZE;
    if (errno != ENOMEM)
        return false;
#endif      /* __APPLE__ */
    stackbottom -= sizeof(void *);
    return stackbottom;
}
#endif      /* __MINGW32__ */

/*
 * Get the top of the stack.
 */
static void __attribute__((noinline)) *gc_stacktop(void)
{
    void *dummy;
    return (void *)&dummy;
}

/*
 * GC initialization.
 */
extern bool GC_init(void)
{
    if (gc_inited)
        return true;    // Already initialised.

    gc_debug("initializing");

    // Check that we are in a 64-bit environment.
    if (sizeof(void *) != sizeof(uint64_t) ||
        sizeof(double) != sizeof(uint64_t))
    {
        errno = ENOEXEC;
        return false;
    }

    // Find the stack:
    gc_stackbottom = gc_get_stackbottom();
    
    // Reserve a large chunk of the virtual address space for the GC.
    void *gc_memory = gc_get_memory();
    if (gc_memory != GC_MEMORY)
        goto init_error;

    // Initialize all of the region information structures.
    for (size_t i = 0; i < GC_NUM_REGIONS; i++)
    {
        void *startptr = GC_MEMORY + i*GC_REGION_SIZE;
        size_t unit = gc_index_unit(i);
        size_t size = (i - gc_unit_offset(unit))*unit + unit;
        uintptr_t offset = (uintptr_t)startptr % size;
        if (offset != 0)
            startptr += size - offset;
        gc_region_t region = __gc_regions + i;
        region->size         = size;
        region->inv_size     = (UINT64_MAX / size) + 1;
        region->freelist     = NULL;
        region->startptr     = startptr;
        region->endptr       = startptr + GC_REGION_SIZE;
        region->freeptr      = startptr;
        region->protectptr   = startptr;
        region->markstartptr = startptr;
        region->markendptr   = startptr;
        region->markptr      = NULL;
        region->startidx     = gc_objidx(startptr);
    }

    // Reserve virtual space for the mark stack.
    gc_markstack = gc_get_mark_memory(GC_MARK_STACK_SIZE);
    if (gc_markstack == NULL)
        goto init_error;

    gc_inited = true;
    return true;

    int saved_errno;
init_error:
    saved_errno = errno;
    if (gc_markstack != NULL)
        gc_free_memory(gc_markstack, GC_MARK_STACK_SIZE);
    if (gc_memory != NULL)
        gc_free_memory(GC_MEMORY, GC_NUM_REGIONS*GC_REGION_SIZE);
    errno = saved_errno;
    return false;
}

/*
 * GC enable/disable.
 */
extern void GC_disable(void)
{
    gc_enabled = false;
}
extern void GC_enable(void)
{
    gc_enabled = true;
}

/*
 * GC root registration.
 */
extern bool GC_root(void *ptr, size_t size)
{
    if (size > GC_MAX_ROOT_SIZE)
    {
        errno = EINVAL;
        return false;
    }
    gc_root_t root = (gc_root_t)malloc(sizeof(struct gc_root_s));
    if (root == NULL)
        return false;
    root->ptr = ptr;
    root->size = size;
    root->ptrptr = &root->ptr;
    root->sizeptr = &root->size;
    root->elemsize = 1;
    gc_add_root(root);
    return true;
}

/*
 * GC dynamic root registration.
 */
extern bool GC_dynamic_root(void **ptrptr, size_t *sizeptr, size_t elemsize)
{
    gc_root_t root = (gc_root_t)malloc(sizeof(struct gc_root_s));
    if (root == NULL)
        return false;
    root->ptr = NULL;
    root->size = 0;
    root->ptrptr = ptrptr;
    root->sizeptr = sizeptr;
    root->elemsize = elemsize;
    gc_add_root(root);
    return true;
}

/*
 * Add a root to the global list.
 */
static void gc_add_root(gc_root_t root)
{
    root->next = gc_roots;
    gc_roots   = root;
}

/*
 * GC set error handler.
 */
extern void GC_error(gc_error_func_t func)
{
    gc_error_func = func;
}

/*
 * GC handle error.
 */
extern void GC_handle_error(bool fatal, int err)
{
    if (err != 0)
        errno = err;
    gc_debug("error occured [fatal=%d, errno=%s]\n", fatal,
        strerror(errno));
    gc_error_func_t func = gc_error_func;
    if (func != NULL)
        func();
    if (fatal)
    {
        fprintf(stderr, "GC fatal error (%s)\n", strerror(errno));
        abort();
    }
}

/*
 * GC should collect?
 */
static inline void gc_maybe_collect(uint32_t size)
{
    gc_alloc_size += size;
    if (gc_alloc_size >= gc_trigger_size)
    {
        if (!gc_enabled)
            return;
        gc_collect();
        size_t gc_scan_size = 0;
        size_t stacksize = gc_stackbottom - gc_stacktop();
        gc_scan_size += 2*stacksize;
        gc_root_t root = gc_roots;
        while (root != NULL)
        {
            gc_scan_size += (*root->sizeptr)*root->elemsize;
            root = root->next;
        }
        gc_scan_size += 2*gc_used_size;
        gc_trigger_size = (size_t)(gc_scan_size / GC_SPACE_FACTOR);
        gc_trigger_size = (gc_trigger_size < GC_MIN_TRIGGER? GC_MIN_TRIGGER:
            gc_trigger_size);
        gc_alloc_size = size;
    }
}

/*
 * GC memory allocation.
 */
extern void *GC_malloc_index(size_t idx)
{
    gc_region_t region = __gc_regions + idx;
    void *ptr;

    // (0) Check if we need to collect.
    gc_maybe_collect(region->size);
    
    // (1) First, attempt to allocate from the freelist.
    gc_freelist_t freelist = region->freelist, next;
    if (freelist != NULL)
    {
nonempty_freelist:
        next = gc_unhide(freelist->next);
        region->freelist = next;
        ptr = (void *)freelist;
        return ptr;
    }

    // (2) Next, attempt to allocated from marked memory.
    if (region->markstartptr < region->markendptr)
    {
        ptr = region->markstartptr;
        uint32_t ptridx = (uint32_t)(gc_objidx(ptr) - region->startidx);
        uint8_t *markptr = region->markptr;
        for (size_t i = 0; i < GC_FREELIST_LEN && ptr < region->markendptr; )
        {
            if (!gc_is_marked_index(markptr, ptridx))
            {
                gc_freelist_t freenode = (gc_freelist_t)ptr;
                freenode->next = gc_hide(freelist);
                freelist = freenode;
                i++;
            }
            ptr += region->size;
            ptridx++;
        }
        region->markstartptr = ptr;
        region->freelist = freelist;
        if (freelist != NULL)
            goto nonempty_freelist;
    }

    // (3) Next, attempt to allocate from fresh space.
    ptr = region->freeptr;
    region->freeptr = ptr + region->size;
    if (ptr >= region->endptr)
    {
        gc_handle_error(false, ENOMEM);
        return NULL;
    }

    // Check if we can access the memory.
    if (ptr + region->size >= region->protectptr)
    {
        void *protectptr = region->protectptr;
        size_t protectlen = GC_PROTECT_LEN*GC_PAGESIZE;
        protectlen = (protectlen < region->size? region->size:
            protectlen);
        if (gc_protect_memory(protectptr, protectlen) != 0)
        {
            gc_debug("protect failed");
            gc_handle_error(false, 0);
            return NULL;
        }
        region->protectptr = protectptr + protectlen;
    }

    return ptr;
}

/*
 * GC memory reallocation.
 */
extern void *GC_realloc(void *ptr, size_t size)
{
    // As per realloc(), if ptr == NULL then gc_realloc() becomes gc_malloc().
    if (ptr == NULL)
        return gc_malloc(size);
    size_t idx_size = gc_size_index(size);
    size_t idx_ptr = gc_index(ptr);
    if (idx_size == idx_ptr)
        return ptr;
    void *newptr = gc_malloc(size);
    if (newptr == NULL)
        return NULL;
    gc_region_t region = __gc_regions + idx_ptr;
    size_t cpy_size = (size < region->size? size: region->size);
    memcpy(newptr, ptr, cpy_size);
    GC_free_nonnull(ptr);
    return newptr;
}

/*
 * GC memory explicit deallocation.
 */
extern void GC_free_nonnull(void *ptr)
{
    // Add ptr to the appropriate freelist.  Do no error checking whatsoever.
    size_t idx = gc_index(ptr);
    gc_region_t region = __gc_regions + idx;
    gc_freelist_t newfreelist = (gc_freelist_t)ptr;
    gc_freelist_t oldfreelist = region->freelist;
    newfreelist->next = gc_hide(oldfreelist);
    region->freelist = newfreelist;
    gc_alloc_size -= (ssize_t)idx;
}

/*
 * GC collection.
 */
extern void GC_collect(void)
{
    // Is collection enabled?
    if (!gc_enabled)
        return;

    // Initialize marking
    gc_debug("collect [stage=init_marks]");
    gc_mark_init();

    gc_debug("collect [stage=mark]");
    struct gc_root_s root_0;
    gc_root_t root = &root_0;
    root->ptr = (void *)gc_stacktop();
    root->size = gc_stackbottom - root->ptr;
    root->ptrptr = &root->ptr;
    root->sizeptr = &root->size;
    root->elemsize = 1;
    root->next = gc_roots;
    gc_root_t roots = root;

    gc_mark(roots);
    gc_sweep();
}

/*
 * Initialize marking.
 */
static void gc_mark_init(void)
{
    gc_total_size = 0;

    // Zero all mark bits.
    for (size_t i = 0; i < GC_NUM_REGIONS; i++)
    {
        gc_region_t region = __gc_regions + i;
        size_t regionsize = region->freeptr - region->startptr;
        if (regionsize == 0)
            continue;
        gc_total_size += regionsize;
        regionsize /= region->size;
        if (region->markptr == NULL)
        {
            size_t marksize = GC_REGION_SIZE / (region->size*8) + GC_PAGESIZE;
            void *markptr = gc_get_mark_memory(marksize);
            if (markptr == NULL)
                gc_handle_error(true, 0);
            region->markptr = (uint8_t *)markptr;
        }
        else
        {
            size_t marksize = (regionsize + 7) / 8;
            gc_zero_memory(region->markptr, marksize);
        }
    }
}

/*
 * Mark the given index.
 */
static inline bool gc_mark_index(uint8_t *markptr_0, uint32_t idx)
{
    gc_markunit_t *markptr = (gc_markunit_t *)markptr_0;
    uint32_t unitidx = (idx / (sizeof(gc_markunit_t)*8));
    gc_write_prefetch(markptr + unitidx);
    uint32_t bitidx  = (idx % (sizeof(gc_markunit_t)*8));
    gc_markunit_t markunit = markptr[unitidx];
    gc_markunit_t markmask = (gc_markunit_t)0x01 << bitidx;
    if (markunit & markmask)
        return false;
    markunit = (markunit | markmask);
    markptr[unitidx] = markunit;
    return true;
}

/*
 * Test if the given index is marked.
 */
static inline bool gc_is_marked_index(uint8_t *markptr_0, uint32_t idx)
{
    gc_markunit_t *markptr = (gc_markunit_t *)markptr_0;
    uint32_t unitidx = (idx / (sizeof(gc_markunit_t)*8));
    gc_read_prefetch(markptr + unitidx);
    uint32_t bitidx  = (idx % (sizeof(gc_markunit_t)*8));
    gc_markunit_t markunit = markptr[unitidx];
    gc_markunit_t markmask = (gc_markunit_t)0x01 << bitidx;
    return ((markunit & markmask) != 0);
}

/*
 * GC marking.
 */
static void gc_mark(gc_root_t roots)
{
    gc_markstack_t stack =
        (gc_markstack_t)(gc_markstack + GC_MARK_STACK_SIZE);
    stack--;
    stack->startptr = NULL;
    stack->endptr   = NULL;

    gc_used_size = 0;
 
    while (true)
    {
        void **ptrptr = stack->startptr;
        void **endptr = stack->endptr;
        if (ptrptr == NULL)
        {
            // Attempt to find some work from the root list.
            if (roots != NULL)
            {
                ptrptr = (void **)*roots->ptrptr;
                size_t size = (*roots->sizeptr)*roots->elemsize;
                endptr = ptrptr + size/sizeof(void *);
                roots = roots->next;
                goto gc_mark_loop_inner;
            }

            gc_debug("collect [stage=sweep]");
            return;
        }
        else
            stack++;

        uint32_t pushed;
gc_mark_loop_inner:

        pushed = 0;
        while (ptrptr < endptr)
        {
            void *ptr = *ptrptr;
            ptrptr++;

            if (!gc_isptr(ptr))
            {
                // 'ptr' is not a value that points to anywhere in the GC's
                // reserved virtual memory addresses; not a GC pointer.
                continue;
            }
            gc_read_prefetch(ptrptr);
            size_t idx = gc_index(ptr);
            gc_region_t region = __gc_regions + idx;
            if (ptr >= region->freeptr || ptr < region->startptr) 
            {
                // 'ptr' points to memory that hasn't been allocated yet, or
                // cannot be collected yet; not a GC pointer.
                continue;
            }

            // 'ptr' has been deemed to be a GC pointer; check if it has been
            // marked;
            uint32_t size = region->size;
            uint32_t ptridx = (uint32_t)(gc_objidx(ptr) - region->startidx);
            if (!gc_mark_index(region->markptr, ptridx))
            {
                // 'ptr' is already marked; no need to follow it.
                continue;
            }
 
            gc_used_size += size;
            ptr = region->startptr + (size_t)ptridx*(size_t)size;
            gc_read_prefetch(ptr);

            // Push onto mark stack:
            stack--;
            stack->startptr = (void **)ptr;
            stack->endptr = (void **)(ptr + size);

            if (pushed > GC_MAX_MARK_PUSH)
            {
                void **tmp_ptrptr = stack[pushed].startptr;
                void **tmp_endptr = stack[pushed].endptr;
                stack[pushed].startptr = ptrptr;
                stack[pushed].endptr   = endptr;
                ptrptr = tmp_ptrptr;
                endptr = tmp_endptr;
                pushed = 0;
            }
            pushed++;
        }
    }
}

/*
 * GC sweeping.
 */
static void gc_sweep(void)
{
    static size_t sweep_count = 0;
    sweep_count++;
    bool returning = (sweep_count % GC_RETURN_SWEEP == 0);

    for (size_t i = 0; i < GC_NUM_REGIONS; i++)
    {
        gc_region_t region = __gc_regions + i;
        if (region->freeptr == region->startptr)
            continue;
        void *ptr = region->freeptr - region->size;
        uint32_t size = region->size;
        uint8_t *markptr = region->markptr;

        if (i == GC_BIG_IDX_OFFSET)
            returning = true;

        // Return memory to the OS:
        int32_t ptridx = (int32_t)(gc_objidx(ptr) - region->startidx);
        int32_t target = ptridx / 2, freesize = 0;
        bool start = true;
        while (true)
        {
            if (ptridx < target || gc_is_marked_index(markptr, ptridx))
            {
                if (freesize >= 3*GC_PAGESIZE)
                {
                    uint32_t offset = size * (ptridx + 1);
                    int32_t diff = offset % GC_PAGESIZE;
                    diff = (diff == 0? 0: GC_PAGESIZE - diff);
                    offset += diff;
                    freesize -= diff;
                    void *freeptr = region->startptr + offset;
                    freesize -= freesize % GC_PAGESIZE;
#ifndef __MINGW32__
                    madvise(freeptr, freesize, MADV_DONTNEED);
#endif      /* __MINGW32__ */
                }
                freesize = 0;
                if (start)
                {
                    void *ptr = region->startptr + size * (ptridx + 1);
                    region->freeptr = ptr;
                    if (!returning)
                        break;
                    start = false;
                }
                if (ptridx < target)
                    break;
            }
            else
                freesize += size;
            ptridx--;
        }

        region->markstartptr = region->startptr;
        region->markendptr = region->freeptr;
        region->freelist = NULL;
    }
}

/*
 * GC strdup()
 */
extern char *GC_strdup(const char *str)
{
    size_t len = strlen(str);
    char *copy = (char *)GC_malloc(len+1);
    strcpy(copy, str);
    return copy;
}

