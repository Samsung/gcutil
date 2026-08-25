# BDWGC Disclaim API — Complete Guide

> This document summarizes knowledge gained about BDWGC's (Boehm-Demers-Weiser
> Garbage Collector) Disclaim API by verifying it against actual code.
> Everything here was confirmed through source analysis and executable tests.

---

## Table of Contents

1. [Disclaim API Overview](#1-disclaim-api-overview)
2. [Basic Usage](#2-basic-usage)
3. [Disclaim Callback: return 0 vs return 1](#3-disclaim-callback-return-0-vs-return-1)
4. [The Free-list Fragment Problem: the Magic Number Pattern](#4-the-free-list-fragment-problem-the-magic-number-pattern)
5. [GC_DEBUG Mode: the Pointer Offset Problem](#5-gc_debug-mode-the-pointer-offset-problem)
6. [Conservative GC and Stale Stack Pointers](#6-conservative-gc-and-stale-stack-pointers)
7. [The mark_from_all Parameter](#7-the-mark_from_all-parameter)
8. [How to Compile](#8-how-to-compile)
9. [Full Example Code](#9-full-example-code)
10. [BDWGC Internal Implementation Analysis](#10-bdwgc-internal-implementation-analysis)
11. [Function and Macro Reference](#11-function-and-macro-reference)
12. [Summary of Common Mistakes](#12-summary-of-common-mistakes)

---

## 1. Disclaim API Overview

### What is Disclaim?

Disclaim is a callback invoked right before an object is reclaimed by the
garbage collector. It resembles a C++ destructor, except that the GC calls it
implicitly.

**Main uses:**
- Automatically closing file handles (`FILE*`)
- Automatically releasing sockets
- Automatically releasing mutexes
- Cleaning up external resources (database connections, etc.)
- Implementing weak-reference tables

### Difference from an ordinary finalizer

| Item | `GC_register_finalizer` | `GC_register_disclaim_proc` |
|------|------------------------|----------------------------|
| When it fires | Separate phase after GC | Inside the GC reclaim phase |
| Performance | Relatively slow | Fast (called directly during sweep) |
| Object resurrection | Possible | Not possible (can only control reclamation) |
| API shape | Registered per object | Registered per kind, in bulk |
| Good fit for | Complex finalization logic | Simple resource cleanup |

---

## 2. Basic Usage

### Headers

```c
#include <gc/gc.h>
#include <gc/gc_mark.h>      /* GC_new_kind, GC_new_free_list, GC_DS_LENGTH,
                                GC_generic_malloc */
#include <gc/gc_disclaim.h>  /* GC_register_disclaim_proc, GC_disclaim_proc */
```

> **Note**: You must include `gc_mark.h`.
> `GC_new_kind`, `GC_new_free_list`, `GC_generic_malloc`, and `GC_DS_LENGTH`
> are all declared/defined in `gc_mark.h`. `gc.h` alone is not enough.

### Minimal example

```c
GC_INIT();

/* 1. Create a new kind */
int my_kind = (int)GC_new_kind(
    GC_new_free_list(),
    GC_DS_LENGTH,   /* descriptor: scan the pointer region using object length */
    1,              /* add_size_to_descriptor */
    0               /* clear_new_objects: do not zero-init */
);

/* 2. Register the disclaim callback */
GC_register_disclaim_proc(my_kind, my_disclaim, 0);

/* 3. Allocate */
MyStruct *obj = (MyStruct *)GC_generic_malloc(sizeof(MyStruct), my_kind);
```

### The GC_DS_LENGTH macro

```c
/* gc_mark.h */
#define GC_DS_LENGTH 0   /* all caps */
```

When the descriptor value is `0` (`GC_DS_LENGTH`), the GC uses the object's
size as the descriptor and conservatively scans the object's pointer region.
This fits ordinary objects that contain pointers.

> **Note**: The macro name is `GC_DS_LENGTH` (all caps).
> `GC_DS_length` (lowercase l) does not exist.

---

## 3. Disclaim Callback: return 0 vs return 1

### The key branch (`reclaim.c:354-366`)

```c
for (bit_no = 0; ADDR_GE(plim, p); bit_no += MARK_BIT_OFFSET(sz)) {
    if (mark_bit_from_hdr(hhdr, bit_no)) {
        p += sz;                    // already marked -> object survives
    } else if (disclaim(p)) {       // <- return 1 (nonzero)
        set_mark_bit_from_hdr(...); //   force the mark bit on!
        INCR_MARKS(hhdr);
        p += sz;                    //   memory is NOT reclaimed
    } else {                        // <- return 0
        obj_link(p) = list;         //   add to the free list
        list = p;
        p = GC_clear_block(p, ...); //   memory IS reclaimed
    }
}
```

### Summary

| Return value | Meaning | GC behavior | Object's fate |
|--------|------|-----------|-----------|
| **0** | "Cleanup done, reclaim it" | Added to the free list, memory reclaimed | **Dies immediately** |
| **1 (nonzero)** | "Not done yet, keep it alive" | Mark bit forced on | **Survives this cycle**, retried on the next GC |

### A real use of return 1 (`tests/weakmap.c:weakmap_disclaim`)

```c
static int GC_CALLBACK
weakmap_disclaim(void *obj_base)
{
    /* ... */

    /* 1. Failed to acquire the lock -> retry next cycle */
    if (weakmap_trylock(wm, h) != 0) {
        return 1;  // <- keep alive
    }

    /* 2. Object is still marked -> retry next cycle */
    if (GC_is_marked(obj_base)) {
        return 1;  // <- keep alive
    }

    /* 3. Cleanup done -> reclaim */
    /* ... remove from the linked list ... */
    return 0;
}
```

### Caveats when using return 1

Header comment (`gc_disclaim.h:40-44`):
> *"at the expense that long chains of objects will take many cycles to reclaim"*

Continually deferring with `return 1` lets an object survive across many GC
cycles. Use it only when genuinely necessary (lock contention, a dependent
object not yet processed, etc.).

### Behavior confirmed by testing

```
Allocate 3 objects (object #1 has should_defer=1)

=== GC Cycle 1 ===
  [Disclaim] Object #2: return 0 -> RECLAIMED     <- reclaimed immediately
  [Disclaim] Object #1: return 1 -> DEFER         <- survives! (mark bit set)
  [Disclaim] Object #0: return 0 -> RECLAIMED     <- reclaimed immediately
After cycle 1: allocated=3, disclaimed=2, deferred=1

=== GC Cycle 2 ===
  [Disclaim] Object #1: return 0 -> RECLAIMED     <- reclaimed on the next cycle
After cycle 2: allocated=3, disclaimed=3, deferred=1
```

---

## 4. The Free-list Fragment Problem: the Magic Number Pattern

### The problem

`GC_disclaim_and_reclaim()` calls the disclaim callback for **every slot in a
heap block that has no mark bit set**. That includes **free-list
fragments** (empty slots).

```
Heap Block
┌──────────┬──────────┬──────────┬──────────┐
│ Slot 0   │ Slot 1   │ Slot 2   │ Slot 3   │
│ (in use) │ (free)   │ (in use) │ (free)   │
│ mark=1   │ mark=0   │ mark=0   │ mark=0   │
└──────────┴──────────┴──────────┴──────────┘
                         ↑           ↑
                    disclaim called  disclaim called
                    (free-list       (free-list
                     link pointer)    link pointer)
```

The first word of a free-list fragment is used as a **link pointer to the
next free object**. Misinterpreting that value as the struct's first field
(e.g. `FILE*`) causes a **segfault**.

### A real crash case

```c
/* [BAD] first field is FILE* -> overlaps the free-list link */
typedef struct {
    FILE *file;        /* <- offset 0: overlaps the free-list link pointer! */
    const char *filename;
} MyResource;

int GC_CALLBACK my_disclaim(void *obj) {
    MyResource *res = (MyResource *)obj;
    if (res->file != NULL) {
        fclose(res->file);  /* <- CRASH: closing the free-list link pointer! */
    }
    return 0;
}
```

GDB backtrace:
```
Program received signal SIGSEGV, Segmentation fault.
0x00007ffff7c85363 in fclose () from /lib/x86_64-linux-gnu/libc.so.6
#1  0x000055555555691b in my_resource_disclaim (obj=0x7ffff7f3e010)
    res->file = 0x7ffff7f3e000  <- free-list link pointer (not a valid FILE*)
```

### The fix: the magic number pattern

Use a **magic number** to distinguish an initialized object from a free-list
fragment. This is the **standard pattern** used by BDWGC's own test code
(`tests/disclaim.c`, `fnlz_mlc.c`).

```c
#define MY_RESOURCE_MAGIC 0xDEADBEEFu

typedef struct {
    unsigned int magic;     /* <- first field: identifies initialization state */
    FILE *file;
    const char *filename;
} MyResource;

int GC_CALLBACK my_disclaim(void *obj) {
    MyResource *res = (MyResource *)obj;

    /* is this a free-list fragment? */
    if (res->magic != MY_RESOURCE_MAGIC) {
        return 0;  /* uninitialized free-list slot -> ignore */
    }

    /* real cleanup logic */
    if (res->file != NULL) {
        fclose(res->file);
        res->file = NULL;
    }
    res->magic = 0;  /* prevent double processing */
    return 0;
}

/* at allocation time */
MyResource *res = GC_generic_malloc(sizeof(MyResource), kind);
res->magic = MY_RESOURCE_MAGIC;  /* <- must be set */
res->file = fopen(...);
```

### The same pattern inside BDWGC itself

**`fnlz_mlc.c:GC_finalized_disclaim`** (uses a flag bit):
```c
STATIC int GC_CALLBACK
GC_finalized_disclaim(void *obj)
{
    ptr_t fc_p = *(ptr_t *)obj;

    if ((ADDR(fc_p) & FINALIZER_CLOSURE_FLAG) != 0) {
        /* real object -> run the finalizer */
        fc->proc(...);
    }
    /* no flag -> free-list fragment -> ignore */
    return 0;
}
```

**`tests/weakmap.c:weakmap_disclaim`** (checks a flag):
```c
static int GC_CALLBACK
weakmap_disclaim(void *obj_base)
{
    header = *(void **)obj_base;
    if (!IS_FLAG_SET(header, FINALIZER_CLOSURE_FLAG)) {
        return 0;  /* free-list fragment -> ignore */
    }
    /* ... */
}
```

---

## 5. GC_DEBUG Mode: the Pointer Offset Problem

### Root cause

When `GC_DEBUG` is defined, the `GC_GENERIC_MALLOC` macro expands to
`GC_debug_generic_malloc` (`gc_mark.h:418-421`):

```c
#ifdef GC_DEBUG
#  define GC_GENERIC_MALLOC(sz, k) GC_debug_generic_malloc(sz, k, GC_EXTRAS)
#else
#  define GC_GENERIC_MALLOC(sz, k) GC_generic_malloc(sz, k)
#endif
```

`GC_debug_generic_malloc` (`dbg_mlc.c:606-614`):
1. Allocates a base via `GC_generic_malloc_aligned(lb + sizeof(oh), ...)`
2. Stores an `oh` debug header at the base
3. `return base + sizeof(oh)` (the user pointer)

But **the disclaim callback always receives the base pointer.**

```
[non-debug mode]
  GC_generic_malloc() returns     = base = start of user data
  disclaim(obj)'s obj             = base = user data  YES matches

[GC_DEBUG mode]
  GC_debug_generic_malloc() returns = base + sizeof(oh) = start of user data
  disclaim(obj)'s obj               = base = start of the oh header  NO offset!
```

### The `oh` debug header layout (`dbg_mlc.h:77-119`)

```c
typedef struct {
    ptr_t oh_back_ptr;           /* back pointer for backtracing */
    const char *oh_string;       /* file name */
    GC_signed_word oh_int;       /* line number */
#ifdef NEED_CALLINFO
    struct callinfo oh_ci[NFRAMES]; /* call stack */
#endif
#ifndef SHORT_DBG_HDRS
    GC_uintptr_t oh_sz;          /* original malloc size */
    GC_uintptr_t oh_sf;          /* start flag (marker) */
#endif
} oh;
```

On a 64-bit system, `sizeof(oh) = 32` bytes.

### Offset confirmed by testing

```
sizeof(oh) = 32

Allocated object #0: user_ptr=0x7d...e0, base_ptr=0x7d...c0, offset=32
Allocated object #1: user_ptr=0x7d...b0, base_ptr=0x7d...90, offset=32
Allocated object #2: user_ptr=0x7d...80, base_ptr=0x7d...60, offset=32
```

### The fix: GC_USR_PTR_FROM_BASE

Use the macro defined in `gc_mark.h:229-231`:

```c
GC_API GC_ATTR_CONST size_t GC_CALL GC_get_debug_header_size(void);
#define GC_USR_PTR_FROM_BASE(p) \
    ((void *)((char *)(p) + GC_get_debug_header_size()))
```

Branch on `#ifdef GC_DEBUG` inside the disclaim callback:

```c
int GC_CALLBACK my_disclaim(void *obj) {
    MyResource *res;
#ifdef GC_DEBUG
    /* GC_DEBUG: obj = start of the oh header -> shift by sizeof(oh) */
    res = (MyResource *)GC_USR_PTR_FROM_BASE(obj);
#else
    /* non-debug: obj = start of user data (no conversion needed) */
    res = (MyResource *)obj;
#endif

    if (res->magic != MY_RESOURCE_MAGIC) {
        return 0;  /* free-list fragment */
    }
    /* ... cleanup logic ... */
    return 0;
}
```

> **On the allocation side**: using the `GC_GENERIC_MALLOC` macro
> automatically selects the right function depending on whether `GC_DEBUG`
> is defined.

### The same pattern inside BDWGC itself (`mark.c:1038-1042`)

```c
#if defined(GC_DEBUG)
    const char *start = GC_USR_PTR_FROM_BASE(addr);
#else
    const char *start = (const char *)addr;
#endif
```

### Test results comparison

| | With `GC_USR_PTR_FROM_BASE` | Without |
|---|---|---|
| Allocated | 5 | 3 |
| Reclaimed via disclaim | **5 (100%)** | **0 (0%)** |
| Magic check | `0xdeadbeef` correct | `0xddcf3000` wrong (misread a field of the oh header) |

---

## 6. Conservative GC and Stale Stack Pointers

### The problem

Because BDWGC is a conservative GC, it interprets every value on the stack as
a potential pointer. Even after you drop a reference to an object, a
**stale pointer value left on the stack** can be misread as a valid
reference and keep the object from being reclaimed.

### A real case from a 2,000-object test

| Action taken | Objects reclaimed |
|------|---------------|
| (none) | 1,997 / 2,000 (3 leaked) |
| Move the allocation logic into a separate function | 1,998 / 2,000 (2 leaked) |
| Add `clear_stack()` | **2,000 / 2,000 (0 leaked)** |

### Fix 1: move it into a separate function

Moving the allocation logic into a separate function cleans up the stack
frame when it returns:

```c
static void
allocate_resources(int kind)
{
    MyResource *res = GC_generic_malloc(sizeof(MyResource), kind);
    res->magic = MY_RESOURCE_MAGIC;
    /* ... */
    res = NULL;  /* drop the reference */
}
/* function returns -> stack frame cleaned up (but stale values can remain) */
```

### Fix 2: clear the stack

Overwrite the previous function's stale pointer values by zeroing a large
local array:

```c
static void
clear_stack(void)
{
    volatile char buf[8192];
    memset((void *)buf, 0, sizeof(buf));
}

/* usage:
 *   allocate_resources(kind);
 *   clear_stack();       <- remove stale stack pointers
 *   GC_gcollect();
 */
```

> **Note**: Use the `volatile` keyword so the compiler cannot optimize this
> away.

### Combining both fixes

Combining both approaches reclaims 100% of 2,000 objects in a single GC
cycle. 10 consecutive runs all confirmed 2,000/2,000 reclaimed.

---

## 7. The mark_from_all Parameter

### The third argument of `GC_register_disclaim_proc`

```c
GC_register_disclaim_proc(int kind, GC_disclaim_proc proc, int mark_from_all);
```

| Value | Meaning | Effect |
|----|------|------|
| **0** | Does not protect objects referenced by the disclaim callback | Fast reclaim |
| **1** | Protects objects referenced by the disclaim callback (`MARK_UNCONDITIONALLY`) | Safe but slower reclaim |

### What happens when `mark_from_all=1` (`allchblk.c:350-354`)

```c
if (ok->ok_disclaim_proc)
    flags |= HAS_DISCLAIM;
if (ok->ok_mark_unconditionally)
    flags |= MARK_UNCONDITIONALLY;
```

When `MARK_UNCONDITIONALLY` is set, the GC mark phase marks the block
unconditionally (`mark.c:2318-2319`):

```c
if ((hhdr->hb_flags & MARK_UNCONDITIONALLY) != 0) {
    GC_push_unconditionally(h, hhdr);
}
```

This can make the object take several GC cycles to be reclaimed.

### Selection criteria

- **`mark_from_all=0`**: the disclaim callback does not reference other GC
  objects (a simple `fclose`, etc.)
- **`mark_from_all=1`**: the disclaim callback does reference other GC
  objects (e.g. managing an inter-object link structure like a weakmap)

### The real danger of misusing `mark_from_all=1`: object resurrection

`mark_from_all=1` does not always end at "safe but slower reclaim" -- in some
cases it means **the object is never reclaimed at all**. If a custom mark
proc (or a kind like `GC_finalized_malloc` that conservatively scans the
whole object via `GC_DS_LENGTH`) reports, while scanning a dead object, a
**pointer back to whatever owns it** as a strong reference:

1. Dead object X gets scanned (because of mark_unconditionally)
2. X's mark result tells the GC "Y is a live reference" (Y being whatever
   strongly owned X, or X itself)
3. The GC marks Y (or X itself)
4. Y's own ordinary graph traversal marks X again (Y -> X is X's normal,
   legitimate ownership direction)
5. Both survive this cycle, and since the heap state is unchanged next
   cycle, steps 1-4 repeat -- **it is never reclaimed**

An ordinary finalizer (the `GC_REGISTER_FINALIZER`/`GC_finalize()` path) at
least detects this shape and prints `WARN("Finalization cycle involving
%p")`, but the disclaim + `mark_unconditionally` path has no such safety net
at all -- it loops forever, silently.

Two real instances found in Escargot:

- `ByteCodeBlockKind`'s mark proc unconditionally reported `m_codeBlock` (a
  pointer back to the owning `InterpretedCodeBlock`)
- `NonSharedBackingStoreKind`/`SharedBackingStoreKind`'s mark proc
  unconditionally reported `m_observerItems` (a pointer back to the
  `ArrayBuffer` that registered itself as an observer)

Both were fixed the same way: trace that field **only when the object itself
is already marked** through the ordinary graph (gated with
`GC_is_marked(GC_base(this))`). A genuinely live object loses nothing, since
ordinary graph traversal marks it anyway; an object visited purely because of
mark_unconditionally (i.e. actually dead) simply drops that field.

`GC_finalized_malloc` (§10.4) has no per-field mark proc at all
(`GC_DS_LENGTH` scans the whole object), so this kind of gating is not even
possible there -- meaning `mark_from_all=0` is the only available defense
whenever the disclaim callback does not reference any other GC object.

---

## 8. How to Compile

### Makefile

```makefile
GC_ROOT = /path/to/bdwgc
GC_LIB  = $(GC_ROOT)/build_test/libgc-lib.a
GC_INC  = $(GC_ROOT)/include

CC      = gcc
CFLAGS  = -g -O0 -I$(GC_INC) -DGC_BUILD=1 -DGC_VISIBILITY_HIDDEN_SET=1
LDFLAGS = -lpthread -ldl

TARGET  = disclaim_example

all: $(TARGET)

$(TARGET): disclaim_example.c
	$(CC) $(CFLAGS) -o $(TARGET) disclaim_example.c $(GC_LIB) $(LDFLAGS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all run clean
```

### Compiling in GC_DEBUG mode

```makefile
# GC_DEBUG mode
CFLAGS  = -g -O0 -I$(GC_INC) -DGC_BUILD=1 -DGC_VISIBILITY_HIDDEN_SET=1 -DGC_DEBUG
```

> **Note**: `GC_DEBUG` must be defined **before** including `gc.h`.
> Either put `#define GC_DEBUG` before `#include <gc/gc.h>` in the source, or
> pass it as a compiler flag.

### Compiler flags explained

| Flag | Purpose |
|--------|------|
| `-DGC_BUILD=1` | Library build setting (access to internal symbols) |
| `-DGC_VISIBILITY_HIDDEN_SET=1` | Symbol visibility setting |
| `-DGC_DEBUG` | Enable debug mode (adds the oh header) |
| `-lpthread` | Thread support |
| `-ldl` | Dynamic loading support |

---

## 9. Full Example Code

### 9.1 Basic example (non-debug mode)

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gc/gc.h>
#include <gc/gc_mark.h>
#include <gc/gc_disclaim.h>

#define MY_RESOURCE_MAGIC 0xDEADBEEFu

typedef struct {
    unsigned int magic;     /* distinguishes free-list fragments */
    FILE *file;
    const char *filename;
} MyResource;

static int g_allocated = 0;
static int g_disclaimed = 0;

int GC_CALLBACK
my_resource_disclaim(void *obj)
{
    MyResource *res = (MyResource *)obj;

    /* ignore free-list fragments */
    if (res->magic != MY_RESOURCE_MAGIC) {
        return 0;
    }

    g_disclaimed++;

    if (res->file != NULL) {
        printf("[Disclaim] Closing file: %s\n", res->filename);
        fclose(res->file);
        res->file = NULL;
    }

    res->magic = 0;  /* prevent double processing */
    return 0;        /* allow the memory to be reclaimed */
}

static void
clear_stack(void)
{
    volatile char buf[8192];
    memset((void *)buf, 0, sizeof(buf));
}

static int g_kind;

static void
allocate_resources(void)
{
    int i;
    for (i = 0; i < 2000; i++) {
        MyResource *r = (MyResource *)GC_generic_malloc(
            sizeof(MyResource), g_kind);
        r->magic = MY_RESOURCE_MAGIC;
        r->filename = "example.txt";
        r->file = (i == 0) ? fopen(r->filename, "w") : NULL;
        g_allocated++;
    }
}

int
main(void)
{
    GC_INIT();

    g_kind = (int)GC_new_kind(GC_new_free_list(),
                               GC_DS_LENGTH, 1, 0);
    GC_register_disclaim_proc(g_kind, my_resource_disclaim, 0);

    allocate_resources();
    clear_stack();

    GC_gcollect();

    printf("Allocated=%d, Disclaimed=%d\n", g_allocated, g_disclaimed);
    return 0;
}
```

### 9.2 GC_DEBUG-compatible example

```c
/*
 * GC_DEBUG-mode-compatible version
 * Compile with: add -DGC_DEBUG
 */
#define GC_DEBUG
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gc/gc.h>
#include <gc/gc_mark.h>
#include <gc/gc_disclaim.h>

#define MY_RESOURCE_MAGIC 0xDEADBEEFu

typedef struct {
    unsigned int magic;
    int id;
} MyResource;

int GC_CALLBACK
my_resource_disclaim(void *obj)
{
    /*
     * GC_DEBUG mode: obj = base pointer (start of the oh header)
     *   -> obtain user data via GC_USR_PTR_FROM_BASE(obj)
     * Non-debug mode: obj = start of user data
     */
#ifdef GC_DEBUG
    MyResource *res = (MyResource *)GC_USR_PTR_FROM_BASE(obj);
#else
    MyResource *res = (MyResource *)obj;
#endif

    if (res->magic != MY_RESOURCE_MAGIC) {
        return 0;  /* free-list fragment */
    }

    printf("[Disclaim] Object #%d reclaimed\n", res->id);
    res->magic = 0;
    return 0;
}

int
main(void)
{
    int kind, i;

    GC_INIT();

    kind = (int)GC_new_kind(GC_new_free_list(), GC_DS_LENGTH, 1, 0);
    GC_register_disclaim_proc(kind, my_resource_disclaim, 0);

    for (i = 0; i < 5; i++) {
        /* GC_GENERIC_MALLOC automatically switches based on GC_DEBUG */
        MyResource *res = (MyResource *)GC_GENERIC_MALLOC(
            sizeof(MyResource), kind);
        res->magic = MY_RESOURCE_MAGIC;
        res->id = i;
    }

    GC_gcollect();
    return 0;
}
```

---

## 10. BDWGC Internal Implementation Analysis

### 10.1 The disclaim call flow

```
GC_gcollect()
  └→ GC_try_to_collect_inner()        (alloc.c:699)
       └→ GC_finish_collection()       (alloc.c:1369)
            └→ GC_start_reclaim()      (reclaim.c:978)
                 └→ GC_apply_to_all_blocks()  (headers.c:363)
                      └→ GC_reclaim_block()   (reclaim.c:655)
                           └→ GC_disclaim_and_reclaim_or_free_small_block()
                                (reclaim.c:520)
                                └→ GC_reclaim_generic()  (reclaim.c:473)
                                     └→ GC_disclaim_and_reclaim()
                                          (reclaim.c:338)
                                      └→ disclaim(p)  ← calls the user callback
```

### 10.2 GC_disclaim_and_reclaim in detail (`reclaim.c:338-369`)

```c
STATIC ptr_t
GC_disclaim_and_reclaim(struct hblk *hbp, hdr *hhdr, size_t sz,
                         ptr_t list, word *pcount)
{
    size_t bit_no;
    ptr_t p, plim;
    int(GC_CALLBACK * disclaim)(void *)
        = GC_obj_kinds[hhdr->hb_obj_kind].ok_disclaim_proc;

    p = hbp->hb_body;
    plim = p + HBLKSIZE - sz;

    for (bit_no = 0; ADDR_GE(plim, p); bit_no += MARK_BIT_OFFSET(sz)) {
        if (mark_bit_from_hdr(hhdr, bit_no)) {
            p += sz;                    /* marked -> survives */
        } else if (disclaim(p)) {       /* return 1 -> survives */
            set_mark_bit_from_hdr(hhdr, bit_no);
            INCR_MARKS(hhdr);
            p += sz;
        } else {                         /* return 0 -> reclaimed */
            obj_link(p) = list;
            list = p;
            p = GC_clear_block(p, sz, pcount);
        }
    }
    return list;
}
```

### 10.3 How the debug header gets added (`dbg_mlc.c`)

```
GC_debug_generic_malloc(lb, kind, GC_EXTRAS)
  │
  ├→ GC_generic_malloc_aligned(lb + sizeof(oh), kind, ...)
  │    └→ obtain the base pointer (space includes the oh header)
  │
  └→ store_debug_info(base, lb, ...)
       ├→ GC_start_debugging_inner()
       │    └→ GC_register_displacement_inner(sizeof(oh))
       │       (interior pointer support: base+sizeof(oh) is also recognized
       │        as a valid pointer)
       │
       ├→ GC_store_debug_info_inner(base, lb, s, i)
       │    └→ record the oh header at base (file name, line number, size,
       │       start flag)
       │
       └→ return base + sizeof(oh)  ← returns the user pointer
```

### 10.4 How GC_finalized_malloc approaches this (`fnlz_mlc.c`)

`GC_finalized_malloc` is a high-level API built on top of the disclaim API.
It stores the finalizer closure pointer in the object's first word, and uses
a flag bit to distinguish free-list fragments.

```c
/* fnlz_mlc.c:28-58 */
STATIC int GC_CALLBACK
GC_finalized_disclaim(void *obj)
{
    ptr_t fc_p = *(ptr_t *)obj;

    if ((ADDR(fc_p) & FINALIZER_CLOSURE_FLAG) != 0) {
        /* flag bit is set -> a real object */
        const struct GC_finalizer_closure *fc
            = (struct GC_finalizer_closure *)CPTR_CLEAR_FLAGS(
                fc_p, FINALIZER_CLOSURE_FLAG);
        fc->proc((ptr_t *)obj + 1, fc->cd);
    }
    /* no flag -> free-list fragment -> ignore */
    return 0;
}
```

It also registers an interior pointer so that `GC_base()` can find the base
from the user pointer:

```c
/* fnlz_mlc.c:91 */
GC_register_displacement_inner(sizeof(ptr_t));
GC_register_displacement_inner(FINALIZER_CLOSURE_FLAG);
GC_register_displacement_inner(sizeof(oh) | FINALIZER_CLOSURE_FLAG);
```

#### Escargot deviation: `GC_finalized_kind` uses `mark_unconditionally=FALSE`

Upstream bdwgc registers `GC_finalized_kind` (the non-atomic one) with
`mark_unconditionally=TRUE`. This repository's `GC_init_finalized_malloc()`
changes that to `FALSE` (see §7, "The real danger of misusing
`mark_from_all=1`: object resurrection").

Reason: every piece of Escargot code that uses `GC_finalized_malloc()`
(`src/intl/Intl*.cpp`, `src/runtime/Temporal*Object.cpp`) only closes a
native ICU handle stored inline in its own object from the disclaim
callback -- none of them has a field that needs to stay valid at disclaim
time because it references another GC object. Meanwhile, keeping
`GC_DS_LENGTH` (whole-object conservative scan) + `mark_unconditionally=TRUE`
means that merely giving one of these JS objects a self-referencing property
(`d.self = d`) makes it unreclaimable forever -- since this kind has no
per-field mark proc to gate, the only available defense is to turn the
feature off entirely. `GC_finalized_ptrfree_kind` (the atomic side,
`GC_finalized_atomic_malloc`) scans no pointers at all, so it has no such
risk and is left as `TRUE`.

This change does not affect `GC_finalized_disclaim()`'s behavior -- that
function only looks at the free-list-fragment flag and always runs
regardless of the `mark_unconditionally` value.

See F15d in `REBASE_PROGRESS.md` for how to reapply this on a rebase.

---

## 11. Function and Macro Reference

### GC_new_kind

```c
/* gc_mark.h:314-318 */
GC_API unsigned GC_CALL GC_new_kind(
    void ** /* free_list */,
    GC_word /* mark_descriptor_template */,
    int /* add_size_to_descriptor */,
    int /* clear_new_objects */
);
```

Creates a new object kind.

| Parameter | Description |
|----------|------|
| `free_list` | The free-list array created by `GC_new_free_list()` |
| `mark_descriptor_template` | The mark descriptor. `GC_DS_LENGTH` (0) means length-based scanning |
| `add_size_to_descriptor` | 1: add the object size to the descriptor. Use 1 for objects containing pointers |
| `clear_new_objects` | 1: zero-initialize new objects. 0: do not initialize |

**Return value**: the new kind ID (cast to `int` for use)

### GC_new_free_list

```c
/* gc_mark.h:307 */
GC_API void **GC_CALL GC_new_free_list(void);
```

Creates and returns a new free-list array.
Pass this as the first argument to `GC_new_kind`.

### GC_register_disclaim_proc

```c
/* gc_disclaim.h:50-52 */
GC_API void GC_CALL GC_register_disclaim_proc(
    int /* kind */,
    GC_disclaim_proc /* proc */,
    int /* mark_from_all */
);
```

Registers a disclaim callback for the given kind.

| Parameter | Description |
|----------|------|
| `kind` | The kind ID created by `GC_new_kind` |
| `proc` | The disclaim callback function, of type `int (*)(void *)` |
| `mark_from_all` | 0: fast reclaim. 1: protect objects the callback references (slower) |

### GC_disclaim_proc (the callback type)

```c
/* gc_disclaim.h:36 */
typedef int(GC_CALLBACK *GC_disclaim_proc)(void * /* obj */);
```

| Return value | Meaning |
|--------|------|
| 0 | Allow the object to be reclaimed |
| nonzero | The object survives this cycle (retried on the next GC) |

### GC_generic_malloc

```c
/* gc_mark.h:385-386 */
GC_API void *GC_CALL GC_generic_malloc(
    size_t /* lb */,
    int /* kind */
);
```

Allocates memory of the given kind.

### GC_GENERIC_MALLOC (macro)

```c
/* gc_mark.h:418-425 */
#ifdef GC_DEBUG
#  define GC_GENERIC_MALLOC(sz, k) GC_debug_generic_malloc(sz, k, GC_EXTRAS)
#else
#  define GC_GENERIC_MALLOC(sz, k) GC_generic_malloc(sz, k)
#endif
```

Automatically selects the right function depending on whether `GC_DEBUG` is
defined. Use this macro instead of `GC_generic_malloc` when writing
GC_DEBUG-compatible code.

### GC_USR_PTR_FROM_BASE

```c
/* gc_mark.h:230-231 */
#define GC_USR_PTR_FROM_BASE(p) \
    ((void *)((char *)(p) + GC_get_debug_header_size()))
```

Converts a base pointer (start of the oh header) into a user data pointer.
Use this inside a disclaim callback in `GC_DEBUG` mode.

### GC_get_debug_header_size

```c
/* gc_mark.h:229 */
GC_API GC_ATTR_CONST size_t GC_CALL GC_get_debug_header_size(void);
```

Returns the size of the debug header (`oh`). 32 bytes on a 64-bit system.

### GC_DS_LENGTH

```c
/* gc_mark.h:104 */
#define GC_DS_LENGTH 0
```

Descriptor value 0. Uses the object length as the descriptor and scans the
pointer region. All caps (not `GC_DS_length`).

### GC_CALLBACK

```c
/* gc_config_macros.h:297 */
#define GC_CALLBACK GC_CALL
```

Specifies the calling convention for callback functions. Used when declaring
a disclaim callback.

### GC_base

```c
/* gc.h */
GC_API void *GC_CALL GC_base(void * /* p */);
```

Returns the base pointer given a user pointer.
In `GC_DEBUG` mode, `base = user_ptr - sizeof(oh)`.
In non-debug mode, `base = user_ptr`.

---

## 12. Summary of Common Mistakes

| # | Mistake | Symptom | Fix |
|---|------|------|--------|
| 1 | `GC_DS_length` (lowercase l) | Compile error | Use `GC_DS_LENGTH` (all caps) |
| 2 | `gc_mark.h` not included | Implicit-declaration warning | `#include <gc/gc_mark.h>` |
| 3 | No magic field | Segfault (free-list link misread as `FILE*`) | Use the magic number pattern |
| 4 | `GC_USR_PTR_FROM_BASE` not used under `GC_DEBUG` | Disclaim callback misreads the oh header | Branch on `#ifdef GC_DEBUG` |
| 5 | Stale stack pointers | Some objects never reclaimed | Split into a separate function + `clear_stack()` |
| 6 | Overusing `mark_from_all=1` | Needs multiple GC cycles (or worse, never reclaimed -- see §7) | Use 0 whenever the callback references no GC object |
| 7 | Calling `GC_generic_malloc` directly in GC_DEBUG mode | No oh header, debug features don't work | Use the `GC_GENERIC_MALLOC` macro |
