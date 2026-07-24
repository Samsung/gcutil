# bdwgc rebase progress (branch: bdwgc_8_3_pre_260715)

## Goal
Rebase our Escargot-custom bdwgc fork onto upstream bdwgc's flat repo layout
(no `bdwgc/` subfolder — files live at repo root, matching upstream since
upstream commit `255d08f6` "Move bdwgc files back to repository root
folder"). Historically our commits were made when the repo had a `bdwgc/`
subfolder (see `9ba5fe9e` "Move files based on our structure"), so their
diffs reference `bdwgc/xxx.c` paths that no longer exist.

Base: `14d2f690` "Update cmake file" (already has the Escargot-custom
CMakeLists.txt, but not yet the GCUtil wrapper sources).

Sibling reference branch: `bdwgc_8_3_pre_main`.

---

## How to use this document (next rebase)

Each feature (F1–F15) is a self-contained **implementation spec**: it describes
what the end state should look like, not a mechanical diff to apply. Upstream
code will have drifted, so line numbers and surrounding context will differ —
read the spec, find the equivalent location in the new upstream by content,
and apply the change.

For reference, each feature also lists:
- `git diff` command: run on THIS branch to see the actual net change
- Commit history: individual commits on THIS branch for fallback

Apply features in dependency order (see graph below). If a feature's spec is
ambiguous, run the `git diff` command and/or inspect the individual commits.

### Rebase order (dependency-sorted)

```
Step  Feature  Description                          Depends on
────  ───────  ───────────────────────────────────  ──────────────
 1    F1       Foundation: bdwgc 7.5-era updates    (base)
 2    F2       Sync to bdwgc release-8.0             F1
 3    F3       Platform macro rename                 F1
 4    F4       32-bit address mode                   F1
 5    F5       Finalizer changes                     (independent)
 6    F6       Debug/release build toggles           (independent)
 7    F7       Data-start detection                  F3
 8    F8       GC_realloc_no_shrink                  (independent)
 9    F9       GCUtil wrapper                        (independent)
10    F10      Thread isolation (GC_THREAD_ISOLATE)  F1, F2
11    F11      TLS access                            F10
12    F12      UFFDWP VDB + monitor thread lifecycle F10
13    F13      Cross-isolate VDB                     F10, F12
14    F14      EAGER_SWEEP conditional enablement    F1
15    F15      Misc new features                     F9 (for F15b only)
16    F16      Raise MAXOBJKINDS under SMALL_CONFIG  (independent)
```

Dependency graph:
```
F1 ─┬─→ F2 ─→ F10 ─┬─→ F11
     │              ├─→ F12 ─→ F13
     │              └─→ (F14 can go anytime after F1)
     ├─→ F3 ─→ F7
     ├─→ F4 (independent)
     ├─→ F5 (independent)
     ├─→ F6 (independent)
     ├─→ F8 (independent)
     └─→ F9 (independent, but needs CMakeLists from base)
F15: standalone (F15b needs F9 for GCUtil.h)
F16: standalone (independent)
```

---

## F1. Foundation: bdwgc 7.5-era base updates

**Depends on:** nothing (foundation)
**Provides for others:** `ok_eager_sweep` field, `GC_n_set_marks` (always-compiled),
custom mark procs, `GC_dump_for_graph`, periodic GC trigger, data-start handling

### What to do

1. **`struct obj_kind` (gc_priv.h):** Add a new `GC_bool ok_eager_sweep;` field
   with comment "Sweep unmarked object immediately." Add `OK_EAGER_SWEEP_INITZ`
   macro as `, FALSE` (used in the static initializer of `GC_obj_kinds[]`).

2. **`GC_n_set_marks` (gc_priv.h + reclaim.c):** Move the declaration and
   definition of `GC_n_set_marks` (and its `count_ones` helper) OUT of the
   `#ifndef NO_DEBUGGING` guard so it is always compiled (both debug and
   release). `GC_count_set_marks_in_hblk` stays debug-only. Rationale:
   `GC_gather_information_for_escargot` calls it unconditionally.

3. **`GC_mark_and_push_custom` (mark.c):** Add custom mark proc. When pushing
   contents, cast `arr[i].from` to `ptr_t` (same as `arr[i].to` already does).

4. **`GC_dump_for_graph` (gc_priv.h):** Add declaration.

5. **`allochblk.c`:** Add periodic GC trigger inside the block allocation path.

6. **`reclaim.c`:** Add `GC_print_block_list()`, `GC_count_set_marks_in_hblk()`,
   and `GC_gather_information_for_escargot()` functions. Do NOT add
   `#define EAGER_SWEEP` at the top of the file — that was a temporary measure
   later replaced by F14's conditional logic.

7. **`GC_init` (misc.c):** Add reset logic for GC state at init.

8. **Data-start handling (os_dep.c):** Add TIZEN/ANDROID data-start detection
   (will be renamed to HOST_TIZEN/HOST_ANDROID in F3).

9. **finalize.c:** Add retry note comment.

10. **Backtrace detail:** Expand backtrace output.

**Skip:** `GC_debug_header_size=0` (upstream made it `const`),
`GC_n_set_marks` reimplementation (upstream already has it).

**Reference:** `git diff 14d2f690..3e266913`
**Commits:** `3e266913` ← `dd51ff197` (squashed: ptr_t cast fix + NO_DEBUGGING guard fix)

---

## F2. Sync to bdwgc release-8.0

**Depends on:** F1

### What to do

1. **`GC_register_mark_stack_func` (gc_mark.h + mark.c):** Add the API function
   that registers a custom mark stack function.

2. **gc_cpp.h:** Simplify the OOM check in `operator new`.

3. **`GC_throw_bad_alloc`:** Do NOT add — gc_badalc.cc is not in the CMake glob,
   so it would cause a link failure.

**Reference:** `git diff 3e266913..e607b16e`
**Commits:** `e607b16e` ← `f1aaad541`

---

## F3. Platform macro rename

**Depends on:** F1
**Provides for:** F7

### What to do

Rename all occurrences of `TIZEN` → `HOST_TIZEN` and `PLATFORM_ANDROID` →
`HOST_ANDROID` across all files (gcconfig.h, os_dep.c, etc.). This matches
upstream's naming convention.

**Reference:** `git diff e607b16e..84c4cd55`
**Commits:** `84c4cd55` ← `5ee41a187`

---

## F4. 32-bit address mode (ESCARGOT_USE_32BIT_IN_64BIT)

**Depends on:** F1

### What to do

1. **`ESCARGOT_USE_32BIT_IN_64BIT` macro (gcconfig.h or gc_priv.h):** Define the
   build flag. When enabled, the GC restricts all allocations to the low 4GB of
   address space. Provide an `#else` fallback with the original plain `mmap` call
   so the code compiles without the flag.

2. **Custom iterable mark proc (mark.c):** Change the signature of the custom
   iterable mark proc to match the 32-bit-in-64bit requirement.

3. **`GC_unix_mmap_get_mem` (os_dep.c):** Add a `MAP_FIXED` loop that searches for
   memory below the 4GB boundary. Use `word` type for address arithmetic (not
   `ptr_t`), and `MAKE_CPTR()` / `ADDR()` for conversions.

4. **mach_dep.c:** Expand 32-bit `jmp_buf` register slots to 64-bit width so the
   register dump captures full 64-bit register values.

5. **os_dep.c mmap retry:** If an `mmap` result crosses the 4GB boundary, retry
   the search starting from 0x1000. Use `MAP_FIXED_NOREPLACE` and a 4GB range.

6. **Windows VirtualAlloc retry (os_dep.c):** On Windows with ClangCL, force
   32-bit address mode by retrying `VirtualAlloc` if the result is above 4GB.
   Adapt to the current (possibly more complex) `VirtualAlloc` call structure.
   **Note:** This path is untested (no Windows toolchain).

**Reference:** `git diff 84c4cd55..9ed82654`
**Commits:** `d764e911` ← `1aea7956f`, `6abeca1e` ← `fadda31f9`,
`b986d735` ← `64a2ee3ad`, `9ed82654` ← `da71abca1`

---

## F5. Finalizer changes

**Depends on:** nothing

### What to do

1. **Disable nested finalizer (finalize.c + pthread_support.c):** Prevent a
   finalizer from triggering another finalizer invocation. Add a guard that
   blocks re-entrant finalizer calls.

2. **Move nest-check into `GC_invoke_finalizers()` (finalize.c):** Move the
   `GC_check_finalizer_nested()` call from the internal helper into the public
   `GC_invoke_finalizers()` API function itself. Wrap the call in `LOCK()`/`UNLOCK()`
   because the THREADS variant of `GC_check_finalizer_nested()` asserts
   `I_HOLD_LOCK()` and the call site previously held no lock.

**Reference:** `git diff 84c4cd55..12daccba -- finalize.c pthread_support.c include/private/gc_priv.h`
**Commits:** `eef01120` ← `897d923e1`, `12daccba` ← `7741a006b`

---

## F6. Debug/release build toggles

**Depends on:** nothing

### What to do

These are mostly independent `#ifdef` / CMake changes. Apply all that are not
already upstream:

1. **`GC_USR_PTR_FROM_BASE` (gc_priv.h):** Gate this macro under `GC_DEBUG` only.
   As a consequence, `start` becomes `const char*` and `proc()` takes `void*`
   (this is upstream `4565459f` — fold it in).

2. **Retry ABORTs (alloc.c):** Remove the `ABORT("Too many retries")` calls in
   `GC_alloc_large` and `GC_allocobj`.

3. **Windows operator-new (gc_cpp.h):** Under `_MSC_VER`, disable operator-new
   overriding.

4. **Interior pointer (CMakeLists.txt):** Add `-DNO_ALL_INTERIOR_POINTERS=1` to
   release build flags. Upstream already has the native `NO_ALL_INTERIOR_POINTERS`
   macro — use it instead of hand-written `#ifdef NO_DEBUGGING` hunks.

5. **`get_have_errors` (gc_priv.h or relevant file):** Under `NO_DEBUGGING`, make
   `get_have_errors()` return FALSE. Apply to the correct non-atomic branch (there
   may be a pre-existing coincidental name collision with a different
   `get_have_errors` gated by `NO_FIND_LEAK && SHORT_DBG_HDRS` — use the right one).

6. **Symbol export (CMakeLists.txt):** Ensure `-DGC_BUILD=1 -DGC_VISIBILITY_HIDDEN_SET=1`
   is present (may already be there from base).

**No-ops (skip entirely):**
- MAXOBJKINDS increase for debug mode — already upstream
- "Heap grown while GC disabled" warning — upstream has `NO_WARN_HEAP_GROW_WHEN_GC_DISABLED` toggle
- stdint.h include fix — already added proactively in F4

**Reference:** `git diff 12daccba..58a8fdf7`
**Commits:** `c99c71bf` ← `602940c03`, `c1d8b18b` ← `9284ebe90`,
`c546e625` ← `056ce8a80`, `c13ed17c` ← `7de13bc7`, `09e8184f` ← `4ea59cd2`,
`58a8fdf7` ← `501c115a`

---

## F7. Data-start detection

**Depends on:** F3 (uses `HOST_TIZEN`/`HOST_ANDROID` macros)

### What to do

In `GC_init_linux_data_start` (os_dep.c):
1. Check `GC_no_dls` BEFORE other data-start detection methods. If `GC_no_dls`
   is set, skip the `dl`-based detection entirely.
2. Remove the `HOST_TIZEN`/`HOST_ANDROID` early-return block that was previously
   short-circuiting data-start detection on those platforms.

**Reference:** `git diff 58a8fdf7..20ef9291 -- os_dep.c`
**Commits:** `20ef9291` ← `f8be9407`

---

## F8. GC_realloc_no_shrink

**Depends on:** nothing

### What to do

1. **mallocx.c:** Add `GC_realloc_no_shrink()` — like `GC_realloc` but never
   shrinks the block. Insert it between `GC_realloc` and `GC_reallocf`.
2. **gc.h:** Add `GC_REALLOC_NO_SHRINK` macro that maps to the function.

**Reference:** `git diff 20ef9291..61163f25`
**Commits:** `61163f25` ← `ec2cbd4f`

---

## F9. GCUtil wrapper

**Depends on:** CMakeLists.txt from base commit `14d2f690`

### What to do

1. **Create wrapper files:**
   - `include/Allocator.h` — Allocator class declaration
   - `Allocator.cpp` — Allocator class implementation (at repo root, not `include/`)
   - `include/GCUtil.h` — Public GCUtil API (includes gc headers, Allocator)
   - `include/GCUtilInternal.h` — Internal helpers
   - `include/LeakChecker.h` / `LeakChecker.cpp` — Leak checker

2. **GCUtil.h includes:** Use `<gc/...>` paths for `gc_mark.h`, `gc_typed.h`,
   `gc_allocator.h`, `gc_backptr.h` (no top-level backward-compat shims exist
   upstream, unlike `gc.h`/`gc_cpp.h`). Keep `#include <gc.h>` as-is.

3. **Allocator.cpp:** Add `#include <cstdio>` (needed for `std::malloc` etc.).

4. **CMakeLists.txt:** Add `NO_WARN_HEAP_GROW_WHEN_GC_DISABLED` to the internal
   cflags. Ensure the wrapper .cpp files are in the build glob.

**Reference:** `git diff 61163f25..88a55e42`
**Commits:** `e7169bd1` ← `9108ca32` (partial), `88a55e42` ← `75448bcb`

---

## F10. Thread isolation (GC_THREAD_ISOLATE)

**Depends on:** F1, F2
**Provides for:** F11, F12, F13

### What to do

This is the largest feature. Goal: make every GC global variable thread-local
(`__thread`) so each thread runs an independent GC instance.

1. **New build flag (gcconfig.h):** Define `GC_THREAD_ISOLATE`. It is mutually
   exclusive with `GC_THREADS`. When set, all `GC_EXTERN` globals become
   `MAY_THREAD_LOCAL` (=`__thread`).

2. **`MAY_THREAD_LOCAL` / `GC_MAY_THREAD_LOCAL` macros (gcconfig.h,
   gc_config_macros.h):** Define them. Under `GC_THREAD_ISOLATE`, they expand to
   `__thread`. Otherwise, they expand to nothing (plain global).

3. **`struct _GC_arrays` (gc_priv.h):** This struct already exists upstream and
   holds many GC globals. Mark it `MAY_THREAD_LOCAL` so each thread gets its own
   copy. Add `GC_data_start` if not present.

4. **All .c files:** Change every `GC_EXTERN` global to `MAY_THREAD_LOCAL GC_EXTERN`.
   Many symbols (GC_root_size, GC_blocked_sp, GC_is_initialized, GC_debugging_started,
   GC_have_errors, GC_manual_vdb, GC_incremental, GC_fail_count, GC_bytes_found,
   GC_reclaimed_bytes_before_gc, etc.) are already inside `struct _GC_arrays`
   upstream — those are automatically covered.

5. **`GC_obj_kinds` static initializer (reclaim.c or wherever defined):** The
   static initializer cannot take the address of thread-local arrays. Zero it out
   and do runtime init in `GC_init()` instead.

6. **`GC_init()` runtime-init block (misc.c):** Add initialization for:
   - `GC_objfreelist_ptr` and all `*_ptr` vars that lost their const+static initializer
   - `GC_obj_kinds[].ok_freelist`
   - Any other vars that were previously statically initialized but now need
     runtime init because they're thread-local

7. **`GC_fl_builder_count`:** This was moved from reclaim.c to mark.c upstream.
   Ensure the extern declaration in gc_priv.h and the definition in mark.c are
   both present and thread-local.

8. **mallocx.c `*_ptr` vars:** These lose their `const` + static initializer when
   made thread-local. They need runtime init (added in step 6).

9. **Audit for missed globals:** After applying all the above, compile all .c
   files with `-DGC_THREAD_ISOLATE=1` and run `readelf -sW` on the output. Grep
   for non-TLS `OBJECT` symbols with `GC_` prefix. Any found must be made
   `MAY_THREAD_LOCAL`. Known gaps found in the original rebase:
   - `GC_quiet` (extern decl in gc_priv.h)
   - `GC_on_os_get_mem` (alloc.c)
   - `GC_has_static_roots` (dyn_load.c — not even in the original diff)
   - `mprotect_vdb_disallowed` (os_dep.c)

10. **`MAY_THREAD_LOCAL volatile`:** Ensure there is a space between
    `MAY_THREAD_LOCAL` and `volatile` (original had `MAY_THREAD_LOCALvolatile`).

### Verification
- Compile all 30 .c files with `-DGC_THREAD_ISOLATE=1`
- `readelf -sW` audit: 0 non-TLS `GC_`-prefixed `OBJECT` symbols
- Plain (non-isolate) build also compiles clean

**Reference:** `git diff 88a55e42..fba57076`
**Commits:** `db50e0c4`–`fba57076` ← `ccadcd110` (6 parts)

---

## F11. TLS access

**Depends on:** F10 (thread isolation must be in place — TLS access is for
thread-local `GC_arrays`)

### What to do

This feature provides two alternative mechanisms to access thread-local
`GC_arrays` without using the compiler's `__thread` keyword directly (needed for
bare-metal / non-pthread environments).

#### Option A: `ENABLE_TLS_ACCESS_BY_ADDRESS`

1. **TLS base address (misc.c or new file):** Implement `GC_tls_base_address()`
   that reads the thread pointer directly via inline asm:
   - x86-64: `movq %fs:0, %rax` (or `%gs` depending on TLS model)
   - x86: `movl %fs:0, %eax`
   - Other arches: see Option B's asm additions

2. **Cached TLS offset (misc.c):** At `GC_init()` time, compute the byte offset
   of `GC_arrays_instance` within the TLS block by subtracting the TLS base from
   its address. Cache this offset. All subsequent accesses to `GC_arrays` fields
   go through `GC_tls_base_address() + cached_offset`.

3. **`GC_obj_kinds` access:** Same approach — cache the offset of
   `GC_obj_kinds_instance` (or use `GC_arrays_instance` if merged, see Option B).

#### Option B: `ENABLE_TLS_ACCESS_BY_PTHREAD_KEY`

1. **Locate pthread TCB key-slot (misc.c):** Write a magic value to the first
   available `pthread_key_t`, then scan the thread's memory near the TCB for that
   magic value. The offset where it's found is the key-slot offset. Cache it.
   Use `getpagesize()` for the scan window size (not a hardcoded 4KB). On LP64,
   use a 64-bit magic constant.

2. **Store `&GC_arrays_instance` (misc.c):** Once per thread, store the address
   of this thread's `GC_arrays_instance` into the located key-slot. Future
   accesses read it back via the cached offset.

3. **Merge `GC_obj_kinds` into `struct _GC_arrays` (gc_priv.h):** Add a
   `GC_obj_kinds_instance[MAXOBJKINDS]` field to `struct _GC_arrays` so only one
   TLS offset is needed for both `GC_arrays` and `GC_obj_kinds`.

4. **`GC_tls_base_address()` asm (misc.c):** Add implementations for:
   - ARM32: appropriate thread-pointer read instruction
   - AArch64: `mrs x0, tpidr_el0`
   - RISCV: appropriate register read
   - I386: fix to `movl` (not `movq`)

#### Both options: `GC_init()` early-return fix

Under either TLS access mode, `GC_is_initialized` (which goes through TLS)
returns garbage at `GC_init()` entry because TLS is not yet set up. Fix: in
those modes, read `GC_arrays_instance._is_initialized` directly (bypassing the
TLS offset cache) for the early-return check.

### Verification
- 30-file compile in release+debug × {no flag, BY_ADDRESS, BY_PTHREAD_KEY}, all under GC_THREAD_ISOLATE
- `readelf -sW` confirms `GC_arrays_instance` / `GC_arrays_pthread_key` land as TLS symbols

**Reference:** `git diff fba57076..425c8c86`
**Commits:** `65ee07f8` ← `4e50abb8`, `0daf8a4f` ← `434bc574`,
`155b19c3` ← `02ce390f`, `425c8c86` (new)

---

## F12. UFFDWP VDB + monitor thread lifecycle

**Depends on:** F10

### What to do

This feature enables userfaultfd-based write-protect VDB (more efficient than
mprotect-based VDB) and makes it work correctly under `GC_THREAD_ISOLATE`.

1. **Enable UFFDWP_VDB (CMakeLists.txt + gcconfig.h):**
   - CMakeLists.txt: For Linux + `GCUTIL_ENABLE_THREADING`, add
     `-DUFFDWP_VDB=1 -DNO_SOFT_VDB=1`.
   - gcconfig.h: Change `!defined(THREADS)` to
     `(!defined(THREADS) && !defined(GC_THREAD_ISOLATE))` in the
     `PREFER_MMAP_PROT_NONE` condition so the mmap PROT_NONE path is also
     active under GC_THREAD_ISOLATE.
   - **Note:** If also applying F13, use F13's CMakeLists.txt version instead
     (uffd-first + mprotect fallback, not uffd-only).

2. **Monitor thread TLS isolation (os_dep.c):**
   `uffdwp_monitor_thread()` runs as a separate OS thread. Under
   `GC_THREAD_ISOLATE`, `uffdwp_fd`, `GC_dirty_pages`, and `GC_page_size` are
   thread-local — the monitor thread would read its own uninitialized copies.
   Fix: create a `uffdwp_monitor_ctx` struct `{ int fd; int stop_fd; word *dirty_pages; size_t page_size; }`,
   heap-allocate it in the owning thread, and pass it through
   `create_detached_thread()`'s new `void *arg` parameter. The monitor thread
   copies the values out and frees the context immediately.
   - `create_detached_thread()`: add `void *arg` parameter, pass to `pthread_create()`.
   - `uffdwp_write_protect()`: add `int fd` parameter, use it instead of the global `uffdwp_fd`.
   - Replace `HBLK_PAGE_ALIGNED()` (which references `GC_page_size`) with
     `PTR_ALIGN_DOWN(addr, page_size)` using the captured page_size.

3. **Make `uffdwp_fd` thread-local (os_dep.c):**
   `static int uffdwp_fd` → `static MAY_THREAD_LOCAL int uffdwp_fd`.
   Safe because step 2 removed the monitor thread's direct reference.

4. **Monitor thread shutdown (os_dep.c + gc_priv.h + misc.c):**
   `GC_deinit()` must stop the monitor thread. Add `GC_dirty_deinit()` called
   from `GC_deinit()`.
   - Give the monitor thread a stop pipe: `uffdwp_stop_wfd` (MAY_THREAD_LOCAL)
     stores the write end.
   - Monitor thread: switch from blocking `read()` to `poll()` on both the uffd
     fd and the stop pipe read end. If the stop pipe fires (`POLLHUP`), break
     the loop, close both fds, and exit.
   - `GC_dirty_deinit()`: closes the pipe write end → wakes `poll()` with `POLLHUP`.
   - Only the monitor thread ever closes the real uffd fd (no close race).

5. **`GC_page_size` plain global (gc_priv.h + os_dep.c):**
   `MAY_THREAD_LOCAL size_t GC_page_size` → `size_t GC_page_size` (and
   `GC_real_page_size` likewise). It is set once from the OS page size and never
   changes per thread. The write-fault handler runs on arbitrary threads that
   may never have called `GC_init()`, so it must not be thread-local.

**Reference:** `git diff 425c8c86..6fffddfb`
**Commits:** `76c6f101`, `4fccf497`, `9375a0a1`, `ebcc9502`, `6fffddfb` (all new)

---

## F13. Cross-isolate VDB

**Depends on:** F10, F12

### What to do

Under `GC_THREAD_ISOLATE`, all GC data structures (dirty bitmap, heap section
list, header table) are thread-local. A plain thread writing into an object
received from another isolate faults, and the write-fault handler cannot
distinguish this from a real segfault — its TLS lookup reports "not found" and
it aborts. The TLS lookup itself can also segfault on threads that never ran
`GC_init()`.

1. **New files: `vdb_isolate.c` / `vdb_isolate.h`:**
   Process-global (non-TLS) registry of heap sections, using `malloc()`/`realloc()`
   dynamic arrays (no fixed cap). Two registries:
   - `range_registry`: records heap section address ranges that are mprotected
     (i.e., the owning thread has incremental GC on).
   - `pending_queue`: pages where a cross-isolate write was detected, waiting for
     the owning isolate to claim them.

2. **Register heap sections (os_dep.c or alloc.c):** When a heap section is
   added to the heap AND the registering thread's `GC_incremental` is on, record
   it in `range_registry` via `mark_range_locked()`.

3. **Write-fault handler (os_dep.c):**
   - First, guard the TLS lookup on `GC_is_initialized` (read directly via
     `GC_arrays_instance` under TLS-by-address/pthread-key modes) to avoid
     segfault-inside-lookup on threads that never ran `GC_init()`.
   - If the TLS lookup misses (page not found in this thread's heap), consult
     `range_registry`. If the faulting address is in another isolate's live
     (committed) page, queue it in `pending_queue` and unprotect the page so the
     write can proceed.
   - The owning isolate claims pending pages at its next `GC_initiate_gc`,
     writing them to its own TLS dirty bitmap.

4. **CMakeLists.txt:** Change Linux + GC_THREAD_ISOLATE from uffd-only
   (`-DNO_MPROTECT_VDB=1 -DUFFDWP_VDB=1 -DNO_SOFT_VDB=1`) to uffd-first +
   mprotect fallback. Do NOT define `NO_MPROTECT_VDB` or `NO_UFFDWP_VDB` —
   let `GC_dirty_init()` try uffd first and fall back to mprotect at runtime.
   Rationale: Tizen app sandbox blocks uffd; forcing uffd-only would leave
   incremental GC silently disabled. Add `-DNO_GWW_VDB` and `-DDONT_PROTECT_PTRFREE=1`.

5. **Registry cleanup on `GC_deinit` (vdb_isolate.c + misc.c):**
   Add `GC_isolate_vdb_deinit()` called from `GC_deinit()` (alongside
   `GC_dirty_deinit()`). It calls `delete_range_locked()` (deletion counterpart
   of `mark_range_locked()`) to remove this thread's heap sections from
   `range_registry`, and drops any `pending_queue` entries within those ranges.
   Must run while `GC_heap_sects`/`GC_n_heap_sects` still describe this thread's
   own heap sections.

**Reference:** `git diff 6fffddfb..efaa911a`
**Commits:** `7c45d3d9`, `efaa911a` (both new)

---

## F14. EAGER_SWEEP conditional enablement

**Depends on:** F1 (provides `ok_eager_sweep` field)

### What to do

F1 originally force-enabled eager sweep via `#define EAGER_SWEEP` at the top of
`reclaim.c`. This feature removes that and makes it per-obj_kind opt-in.

1. **reclaim.c:** Do NOT add `#define EAGER_SWEEP` at the top. (If applying F1
   + F14 together, simply skip the `#define` in F1.)

2. **`GC_reclaim_block` (reclaim.c):** In the condition that decides whether to
   enqueue the block for real reclaim work, add `|| ok->ok_eager_sweep`:
   ```c
   } else if (GC_find_leak_inner || !GC_block_nearly_full(hhdr, sz) || ok->ok_eager_sweep) {
   ```

3. **`GC_reclaim_unconditionally_marked` (reclaim.c):** Change the skip
   condition so blocks with `ok_eager_sweep` are also swept:
   ```c
   if (NULL == rlp)
       continue;
   if (!ok->ok_mark_unconditionally && !ok->ok_eager_sweep)
       continue;
   ```

4. **`GC_enumerate_reachable_objects_inner` (reclaim.c):**
   - Add `GC_is_enumerate_reachable_objects` flag to `struct _GC_arrays` (gc_priv.h):
     `GC_bool _is_enumerate_reachable_objects;` with macro
     `#define GC_is_enumerate_reachable_objects GC_arrays._is_enumerate_reachable_objects`
   - Before enumeration: set the flag TRUE, call `GC_gcollect()` (updates mark
     status), then `GC_reclaim_all((GC_stop_func)0, FALSE)` (flushes dead objects),
     then set the flag FALSE.
   - Wrap the enumeration body in `GC_disable()` / `GC_enable()`.
   - In `GC_start_reclaim()`: if `GC_is_enumerate_reachable_objects` is set,
     call `GC_reclaim_all()` after `GC_apply_to_all_blocks()`.

**Reference:** `git diff efaa911a..2aad00c0 -- reclaim.c include/private/gc_priv.h`
**Commits:** `2aad00c0` (new)

---

## F15. Misc new features

**Depends on:** F9 (F15b needs GCUtil.h for `<gc/gc_disclaim.h>` include)

### What to do

#### F15a. Fix `__stack_base__` declaration (gcconfig.h)

`__stack_base__` is a linker script symbol whose *address* is the initial stack
pointer. Declaring it as `extern void *__stack_base__` is wrong on bare-metal
ARM: the compiler dereferences it, reading 4 bytes from the symbol's address
instead of using the address itself.

Fix: `extern char __stack_base__[];` — array types have no implicit
dereference; the array-to-pointer decay yields the symbol's address directly.

#### F15b. Implement `GC_finalized_atomic_malloc` (fnlz_mlc.c + gc_priv.h + gc_disclaim.h + GCUtil.h)

1. **fnlz_mlc.c:** In `GC_init_finalized_malloc()`, register a new obj kind:
   `GC_finalized_ptrfree_kind = GC_new_kind_inner(GC_new_free_list_inner(), GC_DS_LENGTH, FALSE, TRUE);`
   Register `GC_finalized_disclaim` for it.

2. **fnlz_mlc.c:** Implement `GC_finalized_atomic_malloc(size_t lb, const struct GC_finalizer_closure *fclos)`:
   - Allocate via `GC_malloc_kind(SIZET_SAT_ADD(lb, sizeof(ptr_t)), GC_finalized_ptrfree_kind)`
   - Set the `FINALIZER_CLOSURE_FLAG` on the closure pointer and store it at the
     start of the block (same pattern as `GC_finalized_malloc` but with ptrfree kind)
   - `GC_dirty(op)` + `REACHABLE_AFTER_DIRTY(fc_p)`
   - Return `(ptr_t *)op + 1`

3. **gc_priv.h:** Add `_finalized_ptrfree_kind` field to `struct _GC_arrays`
   with `#define GC_finalized_ptrfree_kind GC_arrays._finalized_ptrfree_kind`.

4. **gc_disclaim.h:** Add `GC_finalized_atomic_malloc` declaration.

5. **GCUtil.h:** Add `#include <gc/gc_disclaim.h>`.

#### F15c. `GC_EVENT_MARK_ABANDON` event (gc.h + alloc.c)

1. **gc.h:** Add `GC_EVENT_MARK_ABANDON` to the `GC_EVENT_TYPE` enum, between
   `GC_EVENT_MARK_END` and `GC_EVENT_RECLAIM_START`.
2. **alloc.c:** In `GC_stopped_mark()`, when `abandoned_at > 0` (incremental
   mark is abandoned), call `GC_on_collection_event(GC_EVENT_MARK_ABANDON)` if
   `GC_on_collection_event` is set. Replaces the existing `/* TODO: Notify
   GC_EVENT_MARK_ABANDON. */` comment.

**Reference:** `git diff 2aad00c0..HEAD`
**Commits:** `1a8009ec`, `55d854fe`, `c70e1ae0` (all new)

---

## F16. Raise MAXOBJKINDS under SMALL_CONFIG

**Depends on:** nothing (independent)

### What to do

**gc_priv.h:** `MAXOBJKINDS`'s `SMALL_CONFIG` branch defines it as 16, versus 24
for a normal (non-debug) build. Escargot registers several custom typed-GC
kinds on top of BDWGC's own built-in kinds (e.g. BackingStore, ByteCodeBlock —
see F9/F15's finalizer work and later Escargot-side history), and 16 isn't
enough headroom once those stack up. Collapse the `SMALL_CONFIG` distinction
so both the small and normal configs get 24, keeping `GC_DEBUG` at 32:

```c
/* Object kinds. */
#ifndef MAXOBJKINDS
#  ifdef GC_DEBUG
#    define MAXOBJKINDS 32
#  else
#    define MAXOBJKINDS 24
#  endif
#endif
```

**Reference:** `git diff 0daf8a4f4^..0daf8a4f4` (the original SMALL_CONFIG=16
commit, on a different branch, before this fix)
**Commits:** (this fix, applied directly on top of whatever branch needs it)

---

## Already upstream (no reapply needed)

These historical hashes were cherry-picked into Samsung/gcutil but are already
reflected in current upstream bdwgc, so skip them entirely:
- `2ea9202ed`
- `f2e2aac0d` (matches the no-op in F6)
- `d3fd2d07b`
- `f52a8050` (partly)
- `d9f5df467` (matches the no-op in F6)

---

## Known follow-up work (not yet done)

- **Windows VirtualAlloc retry-loop** (F4 step 6): never compiled/tested
  (no Windows toolchain available).
- **`test.sh`** (the actual test suite): not yet run — only manual clang
  compile+link+smoke-test verification has been performed. The real CMake
  build has not been invoked this session either.

---

## Workflow conventions

1. Start from the new upstream base.
2. For each feature F1–F15 (in dependency order):
   a. Read the "What to do" spec — understand the intent and end state.
   b. Find the equivalent location in the new upstream by content/context
      (not line numbers — they drift).
   c. Apply the change, adapting to the current code structure.
   d. Run `clang-format -i` on touched files.
   e. Verify `#if/#endif` balance for preprocessor-heavy files (os_dep.c bit us once).
   f. Compile-test (see per-feature verification steps).
   g. Commit as a single commit: `Apply feature <Fx>: <description>`.
3. If the spec is ambiguous, run the `git diff` command (listed per feature) on
   THIS branch to see the actual net change, or inspect individual commits.
4. If a feature conflicts heavily with the new upstream, fall back to the
   individual commit history (listed per feature) and reapply one-by-one.
5. Flag any deviation from the spec to the user in the chat response.
