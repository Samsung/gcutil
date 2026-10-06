/*
 * Copyright (c) 2011 by Hewlett-Packard Company.  All rights reserved.
 *
 * THIS MATERIAL IS PROVIDED AS IS, WITH ABSOLUTELY NO WARRANTY EXPRESSED
 * OR IMPLIED.  ANY USE IS AT YOUR OWN RISK.
 *
 * Permission is hereby granted to use or copy this program
 * for any purpose, provided the above notices are retained on all copies.
 * Permission to modify the code and to distribute modified code is granted,
 * provided the above notices are retained, and a notice that the code was
 * modified is included with the above copyright notice.
 */

#include "private/gc_priv.h"

#ifdef ENABLE_DISCLAIM

#  include "gc/gc_disclaim.h"

GC_INLINE ptr_t *
GC_finalized_closure_slot(ptr_t obj)
{
  const hdr *hhdr;

  GET_HDR(obj, hhdr);
  return (ptr_t *)(obj + hhdr->hb_sz - sizeof(ptr_t));
}

STATIC int GC_CALLBACK
GC_finalized_disclaim(void *obj)
{
  ptr_t *slot = GC_finalized_closure_slot((ptr_t)obj);
#  ifdef AO_HAVE_load
  ptr_t fc_p = GC_cptr_load((volatile ptr_t *)slot);
#  else
  ptr_t fc_p = *slot;
#  endif

  /* Reclaim and explicit free clear every word except the free-list link.
   * Allocations contain at least two words, so the closure cannot be that
   * link even for an empty payload. */
  if (fc_p != NULL) {
    const struct GC_finalizer_closure *fc
        = (const struct GC_finalizer_closure *)fc_p;

    GC_ASSERT(!GC_find_leak_inner);
    fc->proc(obj, fc->cd);
  }
  return 0;
}

STATIC void
GC_register_disclaim_proc_inner(unsigned kind, GC_disclaim_proc proc,
                                GC_bool mark_unconditionally)
{
  GC_ASSERT(kind < MAXOBJKINDS);
  if (UNLIKELY(GC_find_leak_inner))
    return;

  GC_obj_kinds[kind].ok_disclaim_proc = proc;
  GC_obj_kinds[kind].ok_mark_unconditionally = mark_unconditionally;
}

GC_API void GC_CALL
GC_init_finalized_malloc(void)
{
  /* Initialize the collector just in case it is not done yet. */
  GC_init();

  LOCK();
  if (GC_finalized_kind != 0) {
    UNLOCK();
    return;
  }

  GC_finalized_kind
      = GC_new_kind_inner(GC_new_free_list_inner(), GC_DS_LENGTH, TRUE, TRUE);
  GC_ASSERT(GC_finalized_kind != 0);
  /*
   * Escargot note: upstream registers this with mark_unconditionally=TRUE so
   * that fields a finalizer reads stay valid even for a garbage object. We
   * ship it as FALSE instead: every GC_finalized_malloc() caller in this tree
   * only touches native (non-GC) handles from its closure, never a separately
   * GC-managed pointer, so that guarantee buys nothing here. Its cost is real:
   * with mark_unconditionally=TRUE, an unconditional visit to a dead object
   * treats any pointer inside its (conservatively scanned, GC_DS_LENGTH)
   * memory as a live reference -- including one the object's own property
   * storage holds back to itself. A script as simple as
   * `let d = new Intl.DateTimeFormat(); d.self = d; d = null;` then
   * resurrects `d` forever, because the forced scan finds `d.self` and marks
   * `d` reachable again every single cycle. mark_unconditionally=FALSE means
   * this kind is only ever mark-scanned when actually reachable, so a
   * self-referencing island with no external roots is collected normally;
   * GC_finalized_disclaim() still runs during reclaim regardless of this
   * flag, so finalization itself is unaffected.
   */
  GC_register_disclaim_proc_inner(GC_finalized_kind, GC_finalized_disclaim,
                                  FALSE);

  GC_finalized_ptrfree_kind
      = GC_new_kind_inner(GC_new_free_list_inner(), GC_DS_LENGTH, FALSE, TRUE);
  GC_ASSERT(GC_finalized_ptrfree_kind != 0);
  GC_register_disclaim_proc_inner(GC_finalized_ptrfree_kind,
                                  GC_finalized_disclaim, TRUE);
  UNLOCK();
}

GC_API void GC_CALL
GC_register_disclaim_proc(int kind, GC_disclaim_proc proc,
                          int mark_unconditionally)
{
  LOCK();
  GC_register_disclaim_proc_inner((unsigned)kind, proc,
                                  mark_unconditionally != 0);
  UNLOCK();
}

STATIC void *
GC_malloc_finalized(size_t lb, int kind,
                    const struct GC_finalizer_closure *fclos)
{
  void *op;
  ptr_t fc_p;
  ptr_t *slot;
  size_t allocation_size;

#  ifndef LINT2
  /* Actually, there is no data race because the kind is set once. */
  GC_ASSERT(kind != 0);
#  endif
  GC_ASSERT(NONNULL_ARG_NOT_NULL(fclos));
  allocation_size = SIZET_SAT_ADD(lb, sizeof(ptr_t));
  if (allocation_size < 2 * sizeof(ptr_t))
    allocation_size = 2 * sizeof(ptr_t);
  op = GC_malloc_kind(allocation_size, kind);
  if (UNLIKELY(NULL == op))
    return NULL;

  fc_p = (ptr_t)GC_CAST_AWAY_CONST_PVOID(fclos);
  slot = GC_finalized_closure_slot((ptr_t)op);
#  ifdef AO_HAVE_store
  GC_cptr_store((volatile ptr_t *)slot, fc_p);
#  else
  *slot = fc_p;
#  endif
  GC_dirty(slot);
  REACHABLE_AFTER_DIRTY(fc_p);
  return op;
}

GC_API GC_ATTR_MALLOC void *GC_CALL
GC_finalized_malloc(size_t lb, const struct GC_finalizer_closure *fclos)
{
  return GC_malloc_finalized(lb, (int)GC_finalized_kind, fclos);
}

GC_API GC_ATTR_MALLOC void *GC_CALL
GC_finalized_atomic_malloc(size_t lb, const struct GC_finalizer_closure *fclos)
{
  return GC_malloc_finalized(lb, (int)GC_finalized_ptrfree_kind, fclos);
}

#endif /* ENABLE_DISCLAIM */
