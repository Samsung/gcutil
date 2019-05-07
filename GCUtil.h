#ifndef __GCUtil__
#define __GCUtil__

#include <gc.h>
#include <gc_mark.h>
#include <gc_typed.h>
#include <assert.h>
#include <cstdlib>

#ifndef RELEASE_ASSERT
#define RELEASE_ASSERT(assertion)                                                  \
    do {                                                                           \
        if (!(assertion)) {                                                        \
            fprintf(stderr, "RELEASE_ASSERT at %s (%d)\n", __FILE__, __LINE__); \
            abort();                                                               \
        }                                                                          \
    } while (0);
#endif

#ifndef RELEASE_ASSERT_NOT_REACHED
#define RELEASE_ASSERT_NOT_REACHED()                                                       \
    do {                                                                                   \
        fprintf(stderr, "RELEASE_ASSERT_NOT_REACHED at %s (%d)\n", __FILE__, __LINE__); \
        abort();                                                                           \
    } while (0)
#endif

#ifdef ESCARGOT_MEM_STATS
void* GC_malloc_hook(size_t siz);
void* GC_malloc_uncollectable_hook(size_t siz);
void* GC_malloc_atomic_hook(size_t siz);
void* GC_malloc_atomic_uncollectable_hook(size_t siz);
void* GC_malloc_explicitly_typed_hook(size_t siz, GC_descr d);
void* GC_generic_malloc_hook(size_t siz, int kind);
void* GC_malloc_stubborn_hook(size_t siz);
void* GC_strdup_hook(const char* str);
void* GC_strndup_hook(const char* str, size_t siz);
void* GC_realloc_hook(void* address, size_t siz);
void GC_free_hook(void* address);

void GC_register_finalizer_no_order_hook(void* obj, GC_finalization_proc fn,
                                         void* cd, GC_finalization_proc *ofn,
                                         void** ocd);
void GC_print_heap_usage();

#undef GC_MALLOC_EXPLICITLY_TYPED
#define GC_MALLOC_EXPLICITLY_TYPED(bytes, d) GC_malloc_explicitly_typed_hook(bytes, d)

#undef GC_MALLOC
#define GC_MALLOC(X) GC_malloc_hook(X)

#undef GC_MALLOC_UNCOLLECTABLE
#define GC_MALLOC_UNCOLLECTABLE(siz) GC_malloc_uncollectable_hook(siz)

#undef GC_MALLOC_ATOMIC
#define GC_MALLOC_ATOMIC(X) GC_malloc_atomic_hook(X)

#undef GC_MALLOC_ATOMIC_UNCOLLECTABLE
#define GC_MALLOC_ATOMIC_UNCOLLECTABLE(sz) GC_malloc_atomic_uncollectable_hook(sz)

#undef GC_MALLOC_EXPLICITLY_TYPED
#define GC_MALLOC_EXPLICITLY_TYPED(bytes, d) GC_malloc_explicitly_typed_hook(bytes, d)

#undef GC_GENERIC_MALLOC
#define GC_GENERIC_MALLOC(siz, kind) GC_generic_malloc_hook(siz, kind)

#undef GC_MALLOC_STUBBORN
#define GC_MALLOC_STUBBORN(siz) GC_malloc_stubborn_hook(siz)

#undef GC_REALLOC
#define GC_REALLOC(address, siz) GC_realloc_hook(address, siz)

#undef GC_STRDUP
#define GC_STRDUP(str) GC_strdup_hook(str)

#undef GC_STRNDUP
#define GC_STRNDUP(str, siz) GC_strndup_hook(str, siz)

#undef GC_FREE
#define GC_FREE(X) GC_free_hook(X)

#undef GC_REGISTER_FINALIZER_NO_ORDER
#define GC_REGISTER_FINALIZER_NO_ORDER(p, f, d, of, od) GC_register_finalizer_no_order_hook(p, f, d, of, od);

#endif

/* FIXME
 * This is just a workaround to remove ignore_off_page allocator.
 * `ignore_off_page` should be removed from everywhere after stablization.
 */

#undef GC_MALLOC_IGNORE_OFF_PAGE
#define GC_MALLOC_IGNORE_OFF_PAGE GC_MALLOC

#undef GC_MALLOC_ATOMIC_IGNORE_OFF_PAGE
#define GC_MALLOC_ATOMIC_IGNORE_OFF_PAGE GC_MALLOC_ATOMIC

#undef GC_GENERIC_MALLOC_IGNORE_OFF_PAGE
#define GC_GENERIC_MALLOC_IGNORE_OFF_PAGE GC_GENERIC_MALLOC

#include <gc_allocator.h>
#include <gc_cpp.h>
#ifdef GC_DEBUG
#include <gc_backptr.h>
#endif

#include "Allocator.h"

#endif
