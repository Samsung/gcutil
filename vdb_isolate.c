/*
 * Cross-isolate write recovery for GC_THREAD_ISOLATE + MPROTECT_VDB builds.
 * See include/private/vdb_isolate.h for the design.
 *
 * GC_THREAD_ISOLATE makes every other GC data structure (heap sections,
 * dirty bitmap, header table, ...) thread-local, so none of it can be
 * used to recognize a page that belongs to a *different* thread's heap.
 * The registries below are the only process-global (non-TLS) state this
 * scheme needs, and none of it is on any allocation hot path: heap
 * sections are added once per mmap'd chunk, committed/decommitted state
 * only changes when a block is actually (de)committed with the OS, and
 * the pending queue is touched only for faults this thread's own lookup
 * already failed to resolve.
 *
 * Both registries are plain malloc()/realloc()'d arrays, not fixed-size
 * static tables: GC_THREAD_ISOLATE + MPROTECT_VDB already requires an
 * environment with a working mprotect()/SIGSEGV, which means a full
 * libc with malloc() is available too, so there is no reason to impose
 * an arbitrary compile-time cap on how many heap sections or pending
 * faults this scheme can track.
 *
 * GC_write_fault_handler (os_dep.c) takes range_lock/pending_lock from
 * inside a SIGSEGV/SIGBUS handler (or, on ANY_MSWIN, a vectored
 * exception handler running on the faulting thread), so a plain,
 * non-reentrant lock is only safe here because none of these locks'
 * critical sections ever touch VDB-protected memory: a fault can only
 * re-enter this file while the faulting thread already holds one of
 * these locks if that critical section itself takes a fault, and
 * register_heap_sect()/note_unmap()/note_remap()/classify_fault()/
 * drain_pending() only ever read or write the process-global registries
 * below and locals, never a mprotect'd heap page. That rules out
 * same-thread reentrant self-deadlock. Cross-thread deadlock (another
 * isolate pausing this one while it holds a lock) is separately ruled
 * out because GC_THREAD_ISOLATE and GC_THREADS -- hence
 * stop-the-world -- are mutually exclusive at compile time (gcconfig.h).
 *
 * GC_isolate_vdb_push_pending() (the one entry point reached from inside
 * the fault handler) can grow pending_queue via realloc(), which raises
 * a similar question for libc's own allocator lock: could the faulting
 * thread already hold it? No -- the fault here is synchronous on a
 * specific store instruction into a mprotect'd GC heap page, so the
 * thread is executing *that* instruction, not sitting inside malloc's
 * own bookkeeping (which never writes into a GC-managed heap section).
 * A SIGSEGV/SIGBUS on such a store cannot itself originate from inside
 * a malloc() call on the same thread, so realloc() here cannot
 * self-deadlock on libc's allocator lock either.
 */
#include "private/vdb_isolate.h"
#include <stdlib.h>

#if defined(GC_THREAD_ISOLATE) && defined(MPROTECT_VDB)

/* This file's locks guard only the process-global registries below,
   never GC_THREADS-managed state, so they are independent of (and must
   work without) the GC_THREADS-only lock abstraction in gc_locks.h --
   GC_THREAD_ISOLATE and GC_THREADS are mutually exclusive (gcconfig.h).
   windows.h (for ANY_MSWIN builds) is already pulled in by gc_priv.h
   unconditionally, so SRWLOCK is available without linking pthreads,
   which Windows builds of this project do not otherwise use. */
#  ifdef ANY_MSWIN
typedef SRWLOCK GC_isolate_mutex_t;
#    define GC_ISOLATE_MUTEX_INITIALIZER SRWLOCK_INIT
#    define GC_isolate_mutex_lock(m) AcquireSRWLockExclusive(m)
#    define GC_isolate_mutex_unlock(m) ReleaseSRWLockExclusive(m)
#  else
#    include <pthread.h>
typedef pthread_mutex_t GC_isolate_mutex_t;
#    define GC_ISOLATE_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
#    define GC_isolate_mutex_lock(m) pthread_mutex_lock(m)
#    define GC_isolate_mutex_unlock(m) pthread_mutex_unlock(m)
#  endif

/* Starting element count for either registry's backing array, first
   allocated on its first use; both grow by doubling afterward. */
#  ifndef GC_ISOLATE_INITIAL_CAPACITY
#    define GC_ISOLATE_INITIAL_CAPACITY 256
#  endif

/*
 * A single registry of disjoint address ranges covering exactly the
 * union of all heap sections any isolate has ever registered, each
 * tagged with whether it is currently committed (mprotect'd RW, i.e. a
 * live page some isolate can legitimately write to) or decommitted
 * (mprotect(PROT_NONE) + madvise(MADV_DONTNEED) by GC_unmap(), i.e. any
 * write there is a real bug). An address not covered by any range here
 * was never part of any isolate's heap at all.
 *
 * GC_remap() is not guaranteed to be called with the exact same (start,
 * bytes) as the GC_unmap() that decommitted that memory -- blocks can
 * be split/merged in between (allchblk.c) -- so this is modeled as
 * proper interval state, not as a set of (un)commit events paired up by
 * matching address+length. GC_isolate_vdb_note_unmap()/_remap() mark an
 * arbitrary sub-range's committed state, splitting/merging existing
 * entries as needed, so a remap of different shape than the unmap that
 * preceded it still lands on the correct final state everywhere.
 */
typedef struct {
  ptr_t start;
  ptr_t end; /*< exclusive */
  GC_bool committed;
} GC_isolate_range;

/* Marks a page dirty in the *calling* thread's own TLS bitmap, bypassing
   the GC_manual_vdb assertion in GC_dirty_inner() -- this path is only
   ever reached by an isolate draining pages it just confirmed (via
   GC_find_header()) are its own, regardless of whether manual VDB mode
   is enabled for it. Defined in os_dep.c, where async_set_pht_entry_
   from_index()/PHT_HASH() are visible. */
GC_INNER void GC_isolate_dirty_page(const void *p);

static GC_isolate_mutex_t range_lock = GC_ISOLATE_MUTEX_INITIALIZER;
static GC_isolate_range *range_registry = NULL;
static size_t range_count = 0;
static size_t range_capacity = 0;

static GC_isolate_mutex_t pending_lock = GC_ISOLATE_MUTEX_INITIALIZER;
static ptr_t *pending_queue = NULL;
static size_t pending_count = 0;
static size_t pending_capacity = 0;

/* Must be called with range_lock held. Ensures range_registry has room
   for range_count + `needed` entries, growing it via realloc() (by
   doubling) if not. Returns FALSE, leaving range_registry/range_capacity
   untouched, if the allocation fails. */
static GC_bool
ensure_range_capacity_locked(size_t needed)
{
  size_t new_capacity;
  GC_isolate_range *new_registry;

  if (range_count + needed <= range_capacity)
    return TRUE;
  new_capacity
      = 0 == range_capacity ? GC_ISOLATE_INITIAL_CAPACITY : range_capacity * 2;
  while (new_capacity < range_count + needed)
    new_capacity *= 2;
  new_registry = (GC_isolate_range *)realloc(
      range_registry, new_capacity * sizeof(GC_isolate_range));
  if (NULL == new_registry)
    return FALSE;
  range_registry = new_registry;
  range_capacity = new_capacity;
  return TRUE;
}

/* Same as ensure_range_capacity_locked(), for pending_queue; must be
   called with pending_lock held. */
static GC_bool
ensure_pending_capacity_locked(size_t needed)
{
  size_t new_capacity;
  ptr_t *new_queue;

  if (pending_count + needed <= pending_capacity)
    return TRUE;
  new_capacity = 0 == pending_capacity ? GC_ISOLATE_INITIAL_CAPACITY
                                        : pending_capacity * 2;
  while (new_capacity < pending_count + needed)
    new_capacity *= 2;
  new_queue = (ptr_t *)realloc(pending_queue, new_capacity * sizeof(ptr_t));
  if (NULL == new_queue)
    return FALSE;
  pending_queue = new_queue;
  pending_capacity = new_capacity;
  return TRUE;
}

static int
compare_ranges(const void *a, const void *b)
{
  const GC_isolate_range *ra = (const GC_isolate_range *)a;
  const GC_isolate_range *rb = (const GC_isolate_range *)b;
  if (ra->start < rb->start) return -1;
  if (ra->start > rb->start) return 1;
  return 0;
}

/* Merges adjacent (in address, not necessarily in array position)
   same-committed-state entries so range_count does not grow without
   bound under repeated unmap/remap churn. Must be called with
   range_lock held. O(n log n) sorting + O(n) single pass compaction. */
static void
coalesce_ranges_locked(void)
{
  if (range_count <= 1)
    return;

  qsort(range_registry, range_count, sizeof(GC_isolate_range), compare_ranges);

  size_t write_idx = 0;
  size_t i;
  for (i = 1; i < range_count; i++) {
    if (range_registry[write_idx].committed == range_registry[i].committed
        && range_registry[write_idx].end == range_registry[i].start) {
      range_registry[write_idx].end = range_registry[i].end;
    } else {
      write_idx++;
      if (write_idx != i) {
        range_registry[write_idx] = range_registry[i];
      }
    }
  }
  range_count = write_idx + 1;
}

/*
 * Marks [start, end) as `committed` within the registry, splitting any
 * existing entry that only partially overlaps [start, end) so the
 * pre-existing state of the non-overlapping remainder is preserved.
 * Must be called with range_lock held.
 *
 * Entries appended mid-loop (the pieces produced by a split) always end
 * up either fully inside [start, end) -- already carrying the requested
 * `committed` value, so revisiting them later in the same pass is a
 * harmless no-op relabel -- or fully outside it, so they never overlap
 * on a later iteration either. Hence a single forward pass is enough
 * even though range_count changes underneath it.
 */
static void
mark_range_locked(ptr_t start, ptr_t end, GC_bool committed)
{
  size_t i;

  for (i = 0; i < range_count; i++) {
    ptr_t rs = range_registry[i].start;
    ptr_t re = range_registry[i].end;
    GC_bool orig_committed = range_registry[i].committed;

    if (re <= start || rs >= end)
      continue; /*< no overlap */

    if (rs < start && re > end) {
      /* [start, end) strictly inside this entry: split into 3 pieces. */
      if (!ensure_range_capacity_locked(2)) {
        /* Out of memory to represent the split precisely. Relabeling
           the whole entry to `committed` would be unsafe in the
           decommit direction: part of [rs, re) could still be
           genuinely unmapped, and a later cross-isolate write there
           would then be let through as CROSS_ISOLATE instead of
           aborting as DECOMMITTED. Drop the entry entirely instead --
           addresses in [rs, re) then classify as UNKNOWN and fall back
           to the pre-existing abort, which is always the safe
           direction. */
        range_registry[i] = range_registry[range_count - 1];
        range_count--;
        i--; /*< re-examine the entry swapped into this index */
        continue;
      }
      range_registry[i].end = start; /*< left remainder: [rs, start) */
      range_registry[range_count].start = start;
      range_registry[range_count].end = end;
      range_registry[range_count].committed = committed;
      range_count++;
      range_registry[range_count].start = end;
      range_registry[range_count].end = re;
      range_registry[range_count].committed = orig_committed;
      range_count++;
    } else if (rs < start) {
      /* Overlap on this entry's right side: [start, re) is affected,
         and (since not the strictly-inside case) re <= end holds. */
      range_registry[i].end = start; /*< shrink to [rs, start) */
      if (ensure_range_capacity_locked(1)) {
        range_registry[range_count].start = start;
        range_registry[range_count].end = re;
        range_registry[range_count].committed = committed;
        range_count++;
      }
    } else if (re > end) {
      /* Overlap on this entry's left side: [rs, end) is affected, and
         (since rs >= start here) rs >= start holds. */
      range_registry[i].start = end; /*< shrink to [end, re) */
      if (ensure_range_capacity_locked(1)) {
        range_registry[range_count].start = rs;
        range_registry[range_count].end = end;
        range_registry[range_count].committed = committed;
        range_count++;
      }
    } else {
      /* This entry is fully inside [start, end): just relabel it. */
      range_registry[i].committed = committed;
    }
  }
}

/*
 * Removes [start, end) from the registry entirely. Same interval-overlap
 * cases as mark_range_locked() above, except the overlapping portion is
 * dropped rather than relabeled; see GC_isolate_vdb_deinit(), the only
 * caller. Must be called with range_lock held.
 */
static void
delete_range_locked(ptr_t start, ptr_t end)
{
  size_t i;

  for (i = 0; i < range_count; i++) {
    ptr_t rs = range_registry[i].start;
    ptr_t re = range_registry[i].end;

    if (re <= start || rs >= end)
      continue; /*< no overlap */

    if (rs < start && re > end) {
      /* [start, end) strictly inside this entry: keep both remainders. */
      range_registry[i].end = start; /*< left remainder: [rs, start) */
      if (ensure_range_capacity_locked(1)) {
        range_registry[range_count].start = end;
        range_registry[range_count].end = re;
        range_registry[range_count].committed = range_registry[i].committed;
        range_count++;
      }
      /* else: the right remainder [end, re) has to be dropped for lack
         of memory; it then classifies as UNKNOWN, same as if it had
         never been registered -- the safe (if imprecise) direction, as
         in mark_range_locked(). */
    } else if (rs < start) {
      /* Overlap on this entry's right side: keep [rs, start) only. */
      range_registry[i].end = start;
    } else if (re > end) {
      /* Overlap on this entry's left side: keep [end, re) only. */
      range_registry[i].start = end;
    } else {
      /* This entry is fully inside [start, end): drop it. */
      range_registry[i] = range_registry[range_count - 1];
      range_count--;
      i--; /*< re-examine the entry swapped into this index */
    }
  }
}

GC_INNER void
GC_isolate_vdb_register_heap_sect(ptr_t start, size_t bytes)
{
  /* Nothing to recover for a heap that is never write-protected in the
     first place: MPROTECT_VDB only mprotect()s a thread's heap sections
     while that thread's own incremental mode is on, so a cross-isolate
     fault can never occur here unless GC_incremental was already TRUE
     when this section was added. GC_incremental can only go from FALSE
     to TRUE before this thread's own GC_init() completes and never
     afterward (see GC_enable_incremental(), misc.c), so this check is
     stable for the entire lifetime of this thread's GC instance -- no
     section added while it read TRUE can later "lose" its registration
     because incremental turned off, and no section added while it read
     FALSE was ever actually mprotect'd. */
  if (!GC_incremental)
    return;
  GC_isolate_mutex_lock(&range_lock);
  if (ensure_range_capacity_locked(1)) {
    range_registry[range_count].start = start;
    range_registry[range_count].end = start + bytes;
    range_registry[range_count].committed = TRUE;
    range_count++;
    coalesce_ranges_locked();
  }
  /* else: out of memory. classify_fault() will report addresses in this
     section as GC_ISOLATE_VDB_UNKNOWN, i.e. fall back to the
     pre-existing abort behavior -- never a false "safe to ignore". */
  GC_isolate_mutex_unlock(&range_lock);
}

GC_INNER void
GC_isolate_vdb_note_unmap(ptr_t start, size_t bytes)
{
  /* See the comment in GC_isolate_vdb_register_heap_sect(): a section
     never registered (because GC_incremental was off when it was added)
     has nothing to update here either. */
  if (!GC_incremental || NULL == start || 0 == bytes)
    return;
  GC_isolate_mutex_lock(&range_lock);
  mark_range_locked(start, start + bytes, FALSE);
  coalesce_ranges_locked();
  GC_isolate_mutex_unlock(&range_lock);
}

GC_INNER void
GC_isolate_vdb_note_remap(ptr_t start, size_t bytes)
{
  if (!GC_incremental || NULL == start || 0 == bytes)
    return;
  GC_isolate_mutex_lock(&range_lock);
  mark_range_locked(start, start + bytes, TRUE);
  coalesce_ranges_locked();
  GC_isolate_mutex_unlock(&range_lock);
}

GC_INNER GC_isolate_vdb_fault_kind
GC_isolate_vdb_classify_fault(const void *addr)
{
  ptr_t a = (ptr_t)addr;
  GC_isolate_vdb_fault_kind result = GC_ISOLATE_VDB_UNKNOWN;

  GC_isolate_mutex_lock(&range_lock);
  if (range_count > 0) {
    size_t low = 0;
    size_t high = range_count;
    while (low < high) {
      size_t mid = low + (high - low) / 2;
      if (a >= range_registry[mid].start && a < range_registry[mid].end) {
        result = range_registry[mid].committed ? GC_ISOLATE_VDB_CROSS_ISOLATE
                                             : GC_ISOLATE_VDB_DECOMMITTED;
        break;
      } else if (a < range_registry[mid].start) {
        high = mid;
      } else {
        low = mid + 1;
      }
    }
  }
  GC_isolate_mutex_unlock(&range_lock);
  return result;
}

GC_INNER GC_bool
GC_isolate_vdb_push_pending(struct hblk *h, unsigned n)
{
  unsigned i;

  GC_isolate_mutex_lock(&pending_lock);
  if (!ensure_pending_capacity_locked(n)) {
    /* Out of memory: refuse the whole batch rather than drop part of
       it. Silently dropping even one page here would mean the owning
       isolate never marks it dirty and never rescans it, which can
       let the collector free a still-live object (the write may have
       stored a new pointer into an already-black object) -- letting
       the write through unrecorded is unsound, not merely
       "conservative". The caller must fall back to the ordinary abort
       path instead of unprotecting the page. */
    GC_isolate_mutex_unlock(&pending_lock);
    return FALSE;
  }
  for (i = 0; i < n; i++)
    pending_queue[pending_count++] = (ptr_t)(h + i);
  GC_isolate_mutex_unlock(&pending_lock);
  return TRUE;
}

GC_INNER void
GC_isolate_vdb_drain_pending(void)
{
  size_t i;

  if (0 == pending_count)
    return;

  GC_isolate_mutex_lock(&pending_lock);
  i = 0;
  while (i < pending_count) {
    ptr_t addr = pending_queue[i];

    /* GC_find_header() consults this thread's own TLS header table, so
       a hit here means addr truly belongs to this isolate's heap. */
    if (GC_find_header(addr) != NULL) {
      GC_isolate_dirty_page(addr);
      GC_COND_LOG_PRINTF(
          "Claiming cross-isolate dirty page %p into this thread's own"
          " dirty bitmap\n",
          (void *)addr);
      pending_queue[i] = pending_queue[pending_count - 1];
      pending_count--;
      /* Re-check the swapped-in entry at index i; do not advance. */
    } else {
      i++;
    }
  }
  GC_isolate_mutex_unlock(&pending_lock);
}

GC_INNER void
GC_isolate_vdb_deinit(void)
{
  size_t i;

  /* This thread never registered anything if its own incremental mode
     was never on (see GC_isolate_vdb_register_heap_sect()); skip taking
     the locks below for that common case. */
  if (!GC_incremental)
    return;

  GC_isolate_mutex_lock(&range_lock);
  for (i = 0; i < GC_n_heap_sects; i++) {
    delete_range_locked(GC_heap_sects[i].hs_start,
                         GC_heap_sects[i].hs_start + GC_heap_sects[i].hs_bytes);
  }
  coalesce_ranges_locked();
  GC_isolate_mutex_unlock(&range_lock);

  if (0 == pending_count)
    return;

  /* Any pending page inside one of this thread's own heap sections can
     only ever be claimed by this thread (GC_isolate_vdb_drain_pending()
     relies on GC_find_header(), which consults this thread's own TLS
     header table); once that table is gone, it would sit here forever.
     Drop those entries now, while GC_heap_sects/GC_n_heap_sects (about to
     be cleared by GC_deinit()) still identify them. */
  GC_isolate_mutex_lock(&pending_lock);
  i = 0;
  while (i < pending_count) {
    ptr_t addr = pending_queue[i];
    size_t j;
    GC_bool owned = FALSE;

    for (j = 0; j < GC_n_heap_sects; j++) {
      ptr_t hs_start = GC_heap_sects[j].hs_start;
      ptr_t hs_end = hs_start + GC_heap_sects[j].hs_bytes;

      if (addr >= hs_start && addr < hs_end) {
        owned = TRUE;
        break;
      }
    }
    if (owned) {
      pending_queue[i] = pending_queue[pending_count - 1];
      pending_count--;
      /* Re-check the swapped-in entry at index i; do not advance. */
    } else {
      i++;
    }
  }
  GC_isolate_mutex_unlock(&pending_lock);
}

#endif /* GC_THREAD_ISOLATE && MPROTECT_VDB */
