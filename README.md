GC
==

A lightweight conservative Garbage Collector (GC) for C/C++ on 64-bit CPUs.

The basic idea of this GC is as follows: during initialization the GC
allocates (from the operating system) a very large 3072GB memory pool.
This pool never shrinks nor grows, simplifying the overall collector design.
Initially the pool is 100% virtual memory -- so there is no problem
allocating such a large pool on 64bit systems.  Physical memory is only
committed to the pool on demand as it is allocated as used by the program.
This simply uses the underlying demand-paging mechanism of the operating
system.

The simplified memory model has some advantages, such as a significant
simplification of the memory allocation and collection algorithms.  This
reduces the size and complexity of the collector, e.g. the main source file
`gc.c` is less that 1,000SLOC.  There are some additional benefits, such as
optimized implementations some GC API functions, such as `GC_size` and
`GC_base`.

Our implementation is currently only single-threaded.  On the benchmarks we
have tried, our collector is competitive with the more mature Boehm GC, and
can be a lot faster for programs that make use of the optimized GC API
functions.

Systems
-------

Currently 64-bit Linux, MacOSX, and Windows are supported.  The GC compiles
with gcc (Linux/MacOSX) and mingw-64 (Windows).

Building
--------

Our collector is written in `gnu99`, i.e., C99 with GNU extensions.  To
compile simply use `gcc --std=gnu99 -c gc.c`.

Caveats
-------

Unlike the famous Boehm collector, our GC is missing a lot of features.
There are some caveats to be aware of:
* Only 64-bit CPUs are supported.  This is a fundamental limitation of this
  kind of GC.
* Automatic root discovery is not supported.  Roots much be registered
  manually with `GC_root()`.
* No special support for atomic memory.
* Not multi-threaded; only use for single-threaded programs.
* Maximum allocation size is 255MB.
* Allocation of /huge/ objects, i.e. > 1MB, is rounded up to the nearest
  MB boundary, which is not ideal.  In the future we plan to fix this by
  recording the object size for huge allocations.
* The Windows port was the most problematic.  Windows artificially limits the
  usable virtual address space to a measly 8TB (presumably they thought 8TB
  ought to be enough for everybody?), and gets annoyed if you try to
  reserve too much of it.  As a consequence, the memory pool is on Windows is
  limited to 1TB.  This does not cause any problems for most applications.

About
-----

This GC was designed and implemented by Gregory J. Duck.  The official website
is here:

[http://www.comp.nus.edu.sg/~gregory/GC/](http://www.comp.nus.edu.sg/~gregory/GC/)

The main application for this GC is the SMCHR system:

[http://www.comp.nus.edu.sg/~gregory/smchr/](http://www.comp.nus.edu.sg/~gregory/smchr/)

The SMCHR system is a reasonably big project (>20,000 LOC).  Since this GC
design is so lightweight, it is reasonable to incorporate into the SMCHR source
code directly, rather building it as a library and adding an an external
dependency.  This is not generally feasible with the considerably larger Boehm
collector.

Related Work
------------

The fast `GC_size` and `GC_base` operations have some useful applications, such
as for runtime bounds checking of heap allocated objects.  The memory
allocator design (minus the "GC" bit) became the basis of our work on "low fat
pointers" resulting in two publications:
* Gregory J. Duck, Roland H. C. Yap,
  [Heap Bounds Protection with Low Fat Pointers](https://www.comp.nus.edu.sg/~gregory/papers/cc16lowfatptrs.pdf),
  International Conference on Compiler Construction, 2016.
* Gregory J. Duck, Roland H. C. Yap, Lorenzo Cavallaro,
  [Stack Bounds Protection with Low Fat Pointers](https://www.comp.nus.edu.sg/~gregory/papers/ndss17stack.pdf),
  The Network and Distributed System Security Symposium, 2017.

