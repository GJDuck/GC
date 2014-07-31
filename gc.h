/*
 * gc.h
 * Copyright (C) 2012
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

#ifndef __GC_H
#define __GC_H

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define GC_INLINE static inline __attribute__((__always_inline__))
#define GC_CONST  __attribute__((__const__))

/*
 * GC memory base address.
 *
 * This is the base address of all GC memory.  The GC assumes a memory block
 * starting from this address and of suitable size is available when gc_init()
 * is called.  Otherwise gc_init() will fail with an error.
 *
 * This constant can safely be changed to any valid virtual address that
 * satisfies the above assumptions.
 *
 * Note: Windows only uses a 8TB region of virtual address space, so we must
 *       make sure that we are inside of it.
 *
 * DEFAULT: Address 0x200000000
 */
#define GC_MEMORY           ((char *)0x200000000)

/*
 * GC memory region size.
 *
 * This is the size of each GC "region".  A region is an area of the virtual
 * address space reserved for allocating objects of a given size range.
 * Objects from different size ranges are allocated from different regions.
 * If any given region hits this limit, memory allocation will fail.
 *
 * Note: Windows only allows 8TB of address space to be used.  If we use too
 *       much, we seem to bump into artificial limits, so we must use a
 *       reduced bucket size.
 *
 * DEFAULT: 4GB (linux, macosx), 1GB (windows)
 */
#ifndef __MINGW32__
#define GC_REGION_SIZE      ((size_t)4294967296)
#else       /* __MINGW32__ */
#define GC_REGION_SIZE      ((size_t)1073741824)
#endif      /* __MINGW32__ */

/*
 * GC number of memory regions.
 *
 * This determines the total number of memory regions the GC uses.
 *
 * DEFAULT: 768
 */
#define GC_NUM_REGIONS      ((size_t)768)

/*
 * GC memory byte alignment.
 *
 * This determines the address alignment of GC allocated memory.  It also
 * determines the range of allocation sizes for a given region.
 *
 * DEFAULT: 16byte alignment.
 */
#define GC_ALIGNMENT        16

/*
 * GC memory units.
 *
 * This determines the granularity of memory chunks.  GC_UNIT is for small
 * memory allocations, and GC_BIG_UNIT is for large memory allocations.
 */
#define GC_UNIT             GC_ALIGNMENT
#define GC_BIG_UNIT         ((GC_NUM_REGIONS / 3)*GC_UNIT)
#define GC_HUGE_UNIT        ((GC_NUM_REGIONS / 3)*GC_BIG_UNIT)
#define GC_BIG_IDX_OFFSET   (GC_NUM_REGIONS / 3)
#define GC_HUGE_IDX_OFFSET  (2*GC_NUM_REGIONS / 3)

/*
 * GC region information.
 *
 * NOTE: This is PRIVATE!  Do not access/modify directly.
 */
typedef struct gc_freelist_s *gc_freelist_t;
struct gc_region_s
{
    size_t size;                                // Region chunk size.
    size_t inv_size;                            // 1 / size.
    gc_freelist_t freelist;                     // Free-list.
    void *freeptr;                              // Free space pointer.
    void *startptr;                             // Start pointer.
    void *endptr;                               // End pointer.
    void *protectptr;                           // Protect pointer.
    void *markstartptr;                         // Marked (start) pointer.
    void *markendptr;                           // Marked (end) pointer.
    uint8_t *markptr;                           // Mark memory pointer.
    size_t startidx;                            // Start objidx.
};
typedef struct gc_region_s *gc_region_t;
extern struct gc_region_s __gc_regions[GC_NUM_REGIONS];

/*
 * GC hide pointer.
 *
 * Hide/Unhide pointers from the GC.
 */
GC_INLINE void *GC_hide(void *ptr)
{
    return (void *)(~((uintptr_t)ptr));
}
GC_INLINE void *GC_unhide(void *ptr)
{
    return (void *)(~((uintptr_t)ptr));
}
#define gc_hide             GC_hide
#define gc_unhide           GC_unhide

/*
 * Units.
 *
 * Allocation unit sizes.
 */
GC_INLINE size_t GC_index_unit(size_t idx)
{
    return (idx > GC_BIG_IDX_OFFSET?
        (idx > GC_HUGE_IDX_OFFSET? GC_HUGE_UNIT: GC_BIG_UNIT):
         GC_UNIT);
}
GC_INLINE size_t GC_index(void *ptr);
GC_INLINE size_t GC_unit(void *ptr)
{
    return GC_index_unit(GC_index(ptr));
}
GC_INLINE size_t GC_size_unit(size_t size)
{
    return (size > GC_BIG_UNIT?
        (size > GC_HUGE_UNIT? GC_HUGE_UNIT: GC_BIG_UNIT):
         GC_UNIT);
}
GC_INLINE size_t GC_unit_offset(size_t unit)
{
    return (unit == GC_UNIT? 0:
        (unit == GC_BIG_UNIT? GC_BIG_IDX_OFFSET: GC_HUGE_IDX_OFFSET));
}
#define gc_index_unit       GC_index_unit
#define gc_unit             GC_unit
#define gc_size_unit        GC_size_unit
#define gc_unit_offset      GC_unit_offset

/*
 * GC index.
 *
 * The region index.
 */
GC_INLINE size_t GC_index(void *ptr)
{
    return ((size_t)ptr / GC_REGION_SIZE -
        ((size_t)GC_MEMORY / GC_REGION_SIZE));
}
GC_INLINE size_t GC_size_index(size_t size)
{
    uint32_t unit = gc_size_unit(size);
    return ((uint32_t)(size - 1)) / unit + gc_unit_offset(unit);
}
#define gc_index            GC_index
#define gc_size_index       GC_size_index

/*
 * GC region.
 *
 * The region of memory (bucket).
 */
GC_INLINE void *GC_region(void *ptr)
{
    return GC_MEMORY + gc_index(ptr)*GC_REGION_SIZE;
}
GC_INLINE void *GC_index_region(size_t idx)
{
    return GC_MEMORY + idx*GC_REGION_SIZE;
}
GC_INLINE void *GC_size_region(size_t size)
{
    return GC_MEMORY + gc_size_index(size)*GC_REGION_SIZE;
}
#define gc_region           GC_region
#define gc_index_region     GC_index_region
#define gc_size_region      GC_size_region

/*
 * GC size.
 *
 * Object sizes.
 */
GC_INLINE size_t GC_size(void *ptr)
{
    return __gc_regions[GC_index(ptr)].size;
}
GC_INLINE size_t GC_index_size(size_t idx)
{
    return __gc_regions[(idx)].size;
}
#define gc_size             GC_size
#define gc_index_size       GC_index_size

/*
 * GC tagged pointers.
 */
GC_INLINE void *GC_settag(void *ptr, uint32_t tag)
{
    return (char *)ptr + tag;
}
GC_INLINE uint32_t GC_gettag(void *ptr)
{
    return ((uint32_t)(uintptr_t)ptr) & (GC_ALIGNMENT-1);
}
GC_INLINE void *GC_deltag(void *ptr, uint32_t tag)
{
    return (char *)ptr - tag;
}
GC_INLINE void *GC_striptag(void *ptr)
{
    return (char *)ptr - GC_gettag(ptr);
}
#define gc_settag           GC_settag
#define gc_gettag           GC_gettag
#define gc_deltag           GC_deltag
#define gc_striptag         GC_striptag

/*
 * GC extended tagged pointers.
 */
GC_INLINE void *GC_setextag(void *ptr, uint32_t tag)
{
    return (char *)ptr + tag;
}
GC_INLINE void *GC_base(void *ptr);
GC_INLINE uint32_t GC_getextag(void *ptr)
{
    return (uint32_t)((char *)ptr - (char *)GC_base(ptr));
}
GC_INLINE void *GC_delextag(void *ptr, uint32_t tag)
{
    return (char *)ptr - tag;
}
GC_INLINE void *GC_stripextag(void *ptr)
{
    return GC_base(ptr);
}
#define gc_setextag         GC_setextag
#define gc_getextag         GC_getextag
#define gc_delextag         GC_delextag
#define gc_stripextag       GC_stripextag

/*
 * GC is pointer?
 *
 * Tests if the given pointer is a GC pointer or not.
 */
GC_INLINE bool GC_isptr(const void *ptr)
{
    // Deliberate underflow:
    return ((size_t)((char *)ptr - GC_MEMORY) <
        (size_t)GC_NUM_REGIONS*GC_REGION_SIZE);
}
#define gc_isptr            GC_isptr

/*
 * GC object index.
 *
 * Given a pointer, return the object's index with the bucket.
 */
GC_INLINE GC_CONST size_t GC_mul128(size_t x, size_t y)
{
    size_t z;
    asm ("imul %2" : "=d"(z) : "a"(x), "r"(y));
    return z;
}
GC_INLINE uint32_t GC_objidx(void *ptr)
{
    gc_region_t region = __gc_regions + gc_index(ptr);
    return GC_mul128(region->inv_size, (size_t)ptr);
}
#define gc_objidx           GC_objidx

/*
 * GC base pointer.
 *
 * Given an interior pointer to an object, this function returns a pointer to
 * the start of the object.
 */
GC_INLINE void *GC_base(void *ptr)
{
    gc_region_t region = __gc_regions + gc_index(ptr);
    ptr = (void *)(GC_objidx(ptr) * region->size);
    return ptr;
}
#define gc_base             GC_base

/*
 * GC enable/disable.
 */
extern void GC_disable(void);
extern void GC_enable(void);
#define gc_disable          GC_disable
#define gc_enable           GC_enable

/*
 * GC error callback.
 *
 * The GC will call this function for any error, including out-of-memory
 * errors.  If no such function is provided, or the function returns (i.e.
 * does not cause the program to exit), the GC will handle the error in
 * an unspecified way.  Passing 'func = NULL' resets the callback.
 */
typedef void (*gc_error_func_t)(void);
extern void GC_error(gc_error_func_t func);
extern void GC_handle_error(bool fatal, int err);
#define gc_error            GC_error
#define gc_handle_error     GC_handle_error

/*
 * GC initialization.
 *
 * Initializes the GC.  This function should be called before any other GC
 * function, otherwise the behavior of the GC is undefined.  This function
 * should also be called early during program execution, ideally from the
 * main() function, so the GC can accurately determine the base of the stack.
 */
extern bool GC_init(void);
#define gc_init             GC_init

/*
 * GC root registration.
 *
 * Register a root with the GC within the memory range: [ptr .. ptr+size]
 * A root is a region of memory that may contain GC pointers, but itself is
 * not GC allocated memory.  By default, the GC only considers the stack as a
 * root.  Any global variable, or (non-GC) malloc'ed memory, must explicitly be
 * registered as a root if it may contain GC pointers.
 * NOTE: each call to gc_root() consumes additional memory resources, thus
 * should be used sparingly.
 */
extern bool GC_root(void *ptr, size_t size);
#define gc_root             GC_root

/*
 * GC dynamic root registration.
 *
 * This is like GC_root(), except the ptr and the size may be changed by the
 * program at any time.  
 */
extern bool GC_dynamic_root(void **ptrptr, size_t *sizeptr, size_t elemsize);
#define gc_dynamic_root     GC_dynamic_root

/*
 * GC memory allocation.
 *
 * This is the GC's replacement of stdlib malloc().  All memory returned by
 * gc_malloc() is aligned to an address that is a multiple of GC_ALIGNMENT.
 */
extern void *GC_malloc_index(size_t idx) __attribute__((__malloc__));
GC_INLINE void *GC_malloc(size_t size)
{
    // Most of the time 'size' is a constant.  By in-lining, the size->index
    // calculation can be optimized away.
    size_t idx, size1 = size-1;
    if (size1 < GC_BIG_UNIT)
        idx = size1 / GC_UNIT;
    else if (size1 < GC_HUGE_UNIT)
        idx = GC_BIG_IDX_OFFSET + size1 / GC_BIG_UNIT;
    else
    {
        idx = GC_HUGE_IDX_OFFSET + size1 / GC_HUGE_UNIT;
        if (idx >= GC_NUM_REGIONS)
            GC_handle_error(true, EINVAL);
    }
    return GC_malloc_index(idx);
}
#define gc_malloc           GC_malloc

/*
 * GC memory reallocation.
 *
 * This is the GC's replacement of stdlib realloc().  Memory returned by
 * gc_realloc() has the same guarantees as that returned by gc_malloc().
 * After the call to gc_realloc(ptr, size), the memory pointed to by 'ptr'
 * has been explicitly freed, and should no longer be used.
 */
extern void *GC_realloc(void *ptr, size_t size);
#define gc_realloc          GC_realloc

/*
 * GC memory explicit deallocation.
 *
 * This is the GC's replacement of stdlib free().  Using gc_free() is optional.
 */
extern void GC_free_nonnull(void *ptr) __attribute__((__nonnull__(1)));
GC_INLINE void GC_free(void *ptr)
{
    if (ptr == NULL)
        return;
    GC_free_nonnull(ptr);
}
#define gc_free             GC_free

/*
 * GC garbage collection.
 *
 * Force the GC to do a collection.
 */
extern void GC_collect(void) __attribute__((__noinline__));
#define gc_collect          GC_collect

/*
 * GC strdup
 *
 * GC version of stdlib strdup().
 */
extern char *GC_strdup(const char *str);
#define gc_strdup           GC_strdup

#endif      /* __GC_H */
