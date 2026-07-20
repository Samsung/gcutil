/*
 * Cross-isolate write recovery for GC_THREAD_ISOLATE + MPROTECT_VDB builds.
 *
 * A global, non-TLS registry of live heap sections (each tagged
 * committed/decommitted) lets GC_write_fault_handler (os_dep.c) tell a
 * genuine segfault apart from a normal write into another isolate's TLS
 * heap that this thread's own heap-section list has no way to
 * recognize. Recoverable faults are queued and later claimed by the
 * owning isolate at its next GC_initiate_gc (mark.c), which is the only
 * context allowed to touch that isolate's own TLS dirty bitmap.
 *
 * All of the actual bookkeeping lives in vdb_isolate.c; the few call
 * sites wired into alloc.c/os_dep.c/mark.c are one line each so this
 * patch stays easy to drop or rebase independently of upstream bdwgc.
 */
#ifndef GC_VDB_ISOLATE_H
#define GC_VDB_ISOLATE_H

#include "gc_priv.h"

#if defined(GC_THREAD_ISOLATE) && defined(MPROTECT_VDB)

typedef enum {
  /* Not in any registered heap section of any isolate: a real bug. */
  GC_ISOLATE_VDB_UNKNOWN = 0,
  /* In a registered section but inside a currently-decommitted
     sub-range: unmapped memory, also a real bug. */
  GC_ISOLATE_VDB_DECOMMITTED,
  /* In a registered, currently-committed section: a benign write into
     another isolate's live heap. */
  GC_ISOLATE_VDB_CROSS_ISOLATE
} GC_isolate_vdb_fault_kind;

/* Called from GC_add_to_heap() once a heap section is fully installed. */
GC_INNER void GC_isolate_vdb_register_heap_sect(ptr_t start, size_t bytes);

/* Called from GC_unmap()/GC_remap() (os_dep.c) with the same rounded
   [start, start + bytes) range, which os_dep.c already guarantees are
   passed identically to both. */
GC_INNER void GC_isolate_vdb_note_unmap(ptr_t start, size_t bytes);
GC_INNER void GC_isolate_vdb_note_remap(ptr_t start, size_t bytes);

/* Called from the write-fault handler for an address this thread's own
   TLS heap-section lookup did not recognize. */
GC_INNER GC_isolate_vdb_fault_kind GC_isolate_vdb_classify_fault(
    const void *addr);

/* Called from the write-fault handler once GC_isolate_vdb_classify_fault()
   returned GC_ISOLATE_VDB_CROSS_ISOLATE, and BEFORE the page is
   unprotected. Enqueues all `n` consecutive hblk-sized pages starting
   at `h` atomically -- either all of them are queued (returns TRUE, and
   the caller may safely unprotect and let the write retry) or none are
   (returns FALSE, e.g. because the pending queue is full), in which
   case the caller must NOT unprotect and must fall back to the ordinary
   abort path instead of silently letting a write through that the
   owning isolate would never see recorded as dirty. */
GC_INNER GC_bool GC_isolate_vdb_push_pending(struct hblk *h, unsigned n);

/* Called by a thread's own GC_initiate_gc(), before it reads/clears its
   TLS dirty bitmap, to claim any pending pages that belong to it. */
GC_INNER void GC_isolate_vdb_drain_pending(void);

#else /* !(GC_THREAD_ISOLATE && MPROTECT_VDB) */

GC_INLINE void
GC_isolate_vdb_register_heap_sect(ptr_t start, size_t bytes)
{
  (void)start;
  (void)bytes;
}

GC_INLINE void
GC_isolate_vdb_note_unmap(ptr_t start, size_t bytes)
{
  (void)start;
  (void)bytes;
}

GC_INLINE void
GC_isolate_vdb_note_remap(ptr_t start, size_t bytes)
{
  (void)start;
  (void)bytes;
}

GC_INLINE void
GC_isolate_vdb_drain_pending(void)
{
}

#endif /* !(GC_THREAD_ISOLATE && MPROTECT_VDB) */

#endif /* GC_VDB_ISOLATE_H */
