/* Extra testing of the typed allocation API. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_ASSERT(e)                                                    \
  if (!(e)) {                                                             \
    fprintf(stderr, "Assertion failure: %s:%d, %s\n", __FILE__, __LINE__, \
            #e);                                                          \
    exit(1);                                                              \
  }

#define CHECK_OUT_OF_MEMORY(p)            \
  do {                                    \
    if (NULL == (p)) {                    \
      fprintf(stderr, "Out of memory\n"); \
      exit(69);                           \
    }                                     \
  } while (0)

#ifndef NO_TYPED_TEST
#  include "gc/gc_mark.h"
#  include "gc/gc_typed.h"

#  if defined(ESCARGOT_USE_32BIT_IN_64BIT)
#    include <stdint.h>
#    ifdef ENABLE_DISCLAIM
#      include "gc/gc_disclaim.h"
#    endif

#    ifdef ENABLE_DISCLAIM
static void scrub_weak_stack(void);

static const size_t finalized_sizes[] = { 0, 1, 7, 8, 15, 16, 17, 2048,
                                         4096, 65536 };
#      define FINALIZED_SIZE_COUNT \
  (sizeof(finalized_sizes) / sizeof(finalized_sizes[0]))
#      define FINALIZED_REPETITIONS 32
static unsigned finalized_counts[2][FINALIZED_SIZE_COUNT];
static unsigned finalized_freed_count;
static struct GC_finalizer_closure finalized_closures[2][FINALIZED_SIZE_COUNT];

static void GC_CALLBACK
finalized_payload_check(void *obj, void *cd)
{
  size_t id = (size_t)(uintptr_t)cd;
  size_t kind = id % 2, index = id / 2, i;
  const unsigned char *bytes = (const unsigned char *)obj;

  TEST_ASSERT(GC_base(obj) == obj);
  for (i = 0; i < finalized_sizes[index]; ++i)
    TEST_ASSERT(bytes[i] == 0x5a);
  TEST_ASSERT(++finalized_counts[kind][index] <= FINALIZED_REPETITIONS);
}

static void GC_CALLBACK
finalized_freed_check(void *obj, void *cd)
{
  (void)obj;
  (void)cd;
  ++finalized_freed_count;
}

#      if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#      endif
static void
allocate_finalized_payloads(void)
{
  static const struct GC_finalizer_closure freed = { finalized_freed_check,
                                                    NULL };
  size_t kind, index, repeat;

  for (kind = 0; kind < 2; ++kind) {
    for (index = 0; index < FINALIZED_SIZE_COUNT; ++index) {
      struct GC_finalizer_closure *fc = &finalized_closures[kind][index];
      fc->proc = finalized_payload_check;
      fc->cd = (void *)(uintptr_t)(index * 2 + kind);
      for (repeat = 0; repeat < FINALIZED_REPETITIONS; ++repeat) {
        size_t size = finalized_sizes[index];
        void *obj = kind ? GC_finalized_atomic_malloc(size, fc)
                         : GC_finalized_malloc(size, fc);
        CHECK_OUT_OF_MEMORY(obj);
        TEST_ASSERT(GC_base(obj) == obj);
        TEST_ASSERT(((uintptr_t)obj & 7) == 0);
        memset(obj, 0x5a, size);
        GC_reachable_here(obj);
      }
      {
        void *obj = kind ? GC_finalized_atomic_malloc(finalized_sizes[index],
                                                      &freed)
                         : GC_finalized_malloc(finalized_sizes[index], &freed);
        CHECK_OUT_OF_MEMORY(obj);
        GC_FREE(obj);
      }
    }
  }
}

static void
test_finalized_payloads(void)
{
  size_t kind, index, cycle;

  GC_init_finalized_malloc();
  allocate_finalized_payloads();
  for (cycle = 0; cycle < 10; ++cycle) {
    scrub_weak_stack();
    GC_gcollect();
  }
  TEST_ASSERT(finalized_freed_count == 0);
  for (kind = 0; kind < 2; ++kind)
    for (index = 0; index < FINALIZED_SIZE_COUNT; ++index)
      TEST_ASSERT(finalized_counts[kind][index] > 0);
}
#    endif /* ENABLE_DISCLAIM */

static uint32_t
new_compressed_target(void)
{
  void *target = GC_MALLOC(32);
  CHECK_OUT_OF_MEMORY(target);
  return (uint32_t)(uintptr_t)target;
}

#    if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#    endif
static void
register_compressed_weak_target(uint32_t *link)
{
  void *target = GC_MALLOC(32);
  CHECK_OUT_OF_MEMORY(target);
  *link = (uint32_t)(uintptr_t)target;
  TEST_ASSERT(GC_general_register_disappearing_link_compressed(
      link, GC_base(target)) == GC_SUCCESS);
}

#    if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#    endif
static void
scrub_weak_stack(void)
{
  volatile GC_word pad[4096];
  size_t i;
  for (i = 0; i < 4096; ++i)
    pad[i] = 0;
}

static void
test_compressed_weak_link(void)
{
  GC_word bitmap[1] = { 0 };
  const GC_compressed_bitmap_descr *descr;
  struct compressed_weak {
    uint32_t link;
    uint32_t neighbor;
    uint32_t ignored[GC_WORDSZ - 2];
  } *object;
  int i;

  /* Bitmap bits outside the described slots must not retain pointers,
   * even when the corresponding storage is part of the allocation. */
  GC_set_bit(bitmap, GC_WORDSZ - 1);
  descr = GC_make_compressed_bitmap_descriptor(sizeof(*object), bitmap, 2);
  TEST_ASSERT(descr != NULL);
  object = (struct compressed_weak *)GC_malloc_explicitly_typed_compressed(
      sizeof(*object), descr);
  CHECK_OUT_OF_MEMORY(object);
  object->neighbor = 0x12345678U;
  register_compressed_weak_target(&object->link);
  register_compressed_weak_target(&object->ignored[GC_WORDSZ - 3]);
  /* Do not let a pointer left in the stack by registration retain the
   * weak target.  Reading the compressed link before GC can also root it. */
  scrub_weak_stack();
  for (i = 0; i < 20; ++i)
    GC_gcollect();
  TEST_ASSERT(object->link == 0);
  TEST_ASSERT(object->ignored[GC_WORDSZ - 3] == 0);
  TEST_ASSERT(object->neighbor == 0x12345678U);
  GC_FREE(object);
}

static unsigned compressed_finalized;
static void
compressed_bitmap_finalizer(void *obj, void *data)
{
  const uint32_t *slots = (const uint32_t *)obj;
  uintptr_t high = (uintptr_t)obj & ~(uintptr_t)UINT32_MAX;
  (void)data;
  TEST_ASSERT(GC_is_marked((void *)(high | (slots[1] & ~1U))));
  ++compressed_finalized;
}
#    if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#    endif
static void
new_compressed_finalizable(void)
{
  const GC_compressed_bitmap_descr *descr;
  GC_word bitmap[1] = { 0 };
  uint32_t *obj;
  GC_set_bit(bitmap, 1);
  descr = GC_make_compressed_bitmap_descriptor_with_tag(64, bitmap, 2, 1, 1, 0);
  TEST_ASSERT(descr != NULL);
  obj = (uint32_t *)GC_malloc_explicitly_typed_compressed(64, descr);
  CHECK_OUT_OF_MEMORY(obj);
  obj[0] = 0;
  obj[1] = new_compressed_target() | 1U;
  GC_register_finalizer_ignore_self(obj, compressed_bitmap_finalizer,
                                    NULL, NULL, NULL);
}

#    if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#    endif
static void
new_compressed_range_finalizable(unsigned kind)
{
  uint32_t *obj = (uint32_t *)GC_generic_malloc(64, (int)kind);
  CHECK_OUT_OF_MEMORY(obj);
  obj[0] = (uint32_t)(uintptr_t)obj;
  obj[1] = new_compressed_target();
  GC_register_finalizer_ignore_self(obj, compressed_bitmap_finalizer,
                                    NULL, NULL, NULL);
}

static void
test_compressed_range(void)
{
  static const size_t lengths[] = { 1, 2, 3, 255, 256, 257, 5001 };
  unsigned kind = GC_new_kind_32bit();
  size_t n, i;
  uintptr_t high;
  uint32_t *slots, *weak;

  /* Exercise both halves of a native word, continuation boundaries,
   * large objects, and reallocations which must preserve the kind. */
  for (n = 0; n < sizeof(lengths) / sizeof(lengths[0]); ++n) {
    size_t count = lengths[n];
    slots = (uint32_t *)GC_generic_malloc(count * 4, (int)kind);
    CHECK_OUT_OF_MEMORY(slots);
    for (i = 0; i < count; ++i)
      slots[i] = new_compressed_target();
    high = (uintptr_t)slots & ~(uintptr_t)UINT32_MAX;
    scrub_weak_stack();
    GC_gcollect();
    for (i = 0; i < count; ++i)
      TEST_ASSERT(GC_is_marked((void *)(high | slots[i])));
    slots = (uint32_t *)GC_REALLOC(slots, (count + 2) * 4);
    CHECK_OUT_OF_MEMORY(slots);
    slots[count] = new_compressed_target();
    slots[count + 1] = new_compressed_target();
    high = (uintptr_t)slots & ~(uintptr_t)UINT32_MAX;
    scrub_weak_stack();
    GC_gcollect();
    for (i = 0; i < count + 2; ++i)
      TEST_ASSERT(GC_is_marked((void *)(high | slots[i])));
    GC_FREE(slots);
  }

  slots = (uint32_t *)GC_generic_malloc(16, (int)kind);
  weak = (uint32_t *)GC_MALLOC_ATOMIC(4);
  CHECK_OUT_OF_MEMORY(slots);
  CHECK_OUT_OF_MEMORY(weak);
  register_compressed_weak_target(weak);
  slots[0] = *weak | 1U;
  slots[1] = 0;
  slots[2] = 2; /* Reserved first-page immediates are not allocations. */
  slots[3] = 4;
  scrub_weak_stack();
  for (i = 0; i < 20; ++i)
    GC_gcollect();
  TEST_ASSERT(*weak == 0); /* Odd pseudo-pointers must not retain the target. */
  TEST_ASSERT((slots[0] & 1U) != 0);
  GC_FREE(weak);
  GC_FREE(slots);
  {
    unsigned previous = compressed_finalized;
    new_compressed_range_finalizable(kind);
    scrub_weak_stack();
    for (i = 0; i < 20; ++i) {
      GC_gcollect();
      GC_invoke_finalizers();
    }
    TEST_ASSERT(compressed_finalized == previous + 1);
  }
}

static void
test_compressed_bitmap(void)
{
  unsigned previous_finalized = compressed_finalized;
  GC_word bitmap[1] = { 0 };
  const GC_compressed_bitmap_descr *descr;
  struct compressed_slots {
    uint32_t lower;
    uint32_t upper;
  } *object;
  uintptr_t high;

  GC_set_bit(bitmap, 0);
  descr = GC_make_compressed_bitmap_descriptor(sizeof(*object), bitmap, 2);
  TEST_ASSERT(descr != NULL);
  TEST_ASSERT(GC_make_compressed_bitmap_descriptor(sizeof(*object),
      bitmap, 3) == NULL);
  object = (struct compressed_slots *)GC_malloc_explicitly_typed_compressed(
      sizeof(*object), descr);
  CHECK_OUT_OF_MEMORY(object);
  TEST_ASSERT(GC_malloc_explicitly_typed_compressed(sizeof(*object) - 1,
                                                    descr) == NULL);
  object->lower = new_compressed_target();
  object->upper = 0;
  high = (uintptr_t)object & ~(uintptr_t)UINT32_MAX;
  GC_gcollect();
  TEST_ASSERT(GC_is_marked((void *)(high | object->lower)));
  GC_FREE(object);

  bitmap[0] = 0;
  GC_set_bit(bitmap, 1);
  descr = GC_make_compressed_bitmap_descriptor(sizeof(*object), bitmap, 2);
  TEST_ASSERT(descr != NULL);
  object = (struct compressed_slots *)GC_malloc_explicitly_typed_compressed(
      sizeof(*object), descr);
  CHECK_OUT_OF_MEMORY(object);
  object->lower = 0;
  object->upper = new_compressed_target();
  high = (uintptr_t)object & ~(uintptr_t)UINT32_MAX;
  GC_gcollect();
  TEST_ASSERT(GC_is_marked((void *)(high | object->upper)));
  GC_FREE(object);

  descr = GC_make_compressed_bitmap_descriptor_with_tag(sizeof(*object),
      bitmap, 2, 1, 1, 0);
  TEST_ASSERT(descr != NULL);
  TEST_ASSERT(GC_make_compressed_bitmap_descriptor_with_tag(sizeof(*object),
      bitmap, 2, 0, 1, 0) == NULL);
  TEST_ASSERT(GC_make_compressed_bitmap_descriptor_with_tag(sizeof(*object),
      bitmap, 2, 2, 1, 0) == NULL);
  object = (struct compressed_slots *)GC_malloc_explicitly_typed_compressed(
      sizeof(*object), descr);
  CHECK_OUT_OF_MEMORY(object);
  object->lower = 0;
  object->upper = new_compressed_target() | 1U;
  high = (uintptr_t)object & ~(uintptr_t)UINT32_MAX;
  GC_gcollect();
  TEST_ASSERT((object->upper & 1U) != 0);
  TEST_ASSERT(GC_is_marked((void *)(high | (object->upper & ~1U))));
  object->upper = 1; /* Tagged empty must not retain a heap object. */
  GC_gcollect();
  TEST_ASSERT(object->upper == 1);
  GC_FREE(object);
  new_compressed_finalizable();
  scrub_weak_stack();
  GC_gcollect();
  GC_invoke_finalizers();
  TEST_ASSERT(compressed_finalized == previous_finalized + 1);
  {
    GC_word wide_bitmap[2] = { 0, 0 };
    uint32_t *wide;
    GC_set_bit(wide_bitmap, 0);
    GC_set_bit(wide_bitmap, GC_WORDSZ - 1);
    GC_set_bit(wide_bitmap, GC_WORDSZ);
    descr = GC_make_compressed_bitmap_descriptor(
        (GC_WORDSZ + 1) * 4, wide_bitmap, GC_WORDSZ + 1);
    TEST_ASSERT(descr != NULL);
    wide = (uint32_t *)GC_malloc_explicitly_typed_compressed(
        (GC_WORDSZ + 1) * 4, descr);
    CHECK_OUT_OF_MEMORY(wide);
    memset(wide, 0, (GC_WORDSZ + 1) * 4);
    wide[0] = new_compressed_target();
    wide[GC_WORDSZ - 1] = new_compressed_target();
    wide[GC_WORDSZ] = new_compressed_target();
    high = (uintptr_t)wide & ~(uintptr_t)UINT32_MAX;
    GC_gcollect();
    TEST_ASSERT(GC_is_marked((void *)(high | wide[0])));
    TEST_ASSERT(GC_is_marked((void *)(high | wide[GC_WORDSZ - 1])));
    TEST_ASSERT(GC_is_marked((void *)(high | wide[GC_WORDSZ])));
    GC_FREE(wide);
  }

}
#  endif

#  define ROUNDUP_WORDSZ(s) (((s) + GC_WORDSZ - 1) / GC_WORDSZ)

/* Test basic functionality with small bitmap. */
static void
test_small_bitmap(void)
{
  GC_word bm[2] = { 0 };

  GC_set_bit(bm, 0);
  GC_set_bit(bm, 3);
  GC_set_bit(bm, 7);

  /* This should not trigger `GC_add_ext_descriptor()` since it is small. */
  TEST_ASSERT(GC_make_descriptor(bm, 8) != 0);
}

/* Test a large bitmap that should force use of ext descriptor. */
static void
test_large_bitmap(void)
{
  const size_t large_size = 2000; /*< greater than BITMAP_BITS */
  GC_descr d;
  size_t bm_sz = ROUNDUP_WORDSZ(large_size) * sizeof(GC_word);
  GC_word *bm = (GC_word *)GC_MALLOC_ATOMIC(bm_sz);
  void **p;

  CHECK_OUT_OF_MEMORY(bm);
  memset(bm, 0, bm_sz);

  /* Set some scattered bits to simulate pointer fields. */
  GC_set_bit(bm, 0);
  GC_set_bit(bm, 15);
  GC_set_bit(bm, 100);
  GC_set_bit(bm, 255);
  GC_set_bit(bm, 500);
  GC_set_bit(bm, 750);
  GC_set_bit(bm, 999);
  GC_set_bit(bm, 1500);
  GC_set_bit(bm, 1999);

  /* This should trigger `GC_add_ext_descriptor()` internally. */
  d = GC_make_descriptor(bm, large_size);

  TEST_ASSERT(d != 0);
  TEST_ASSERT((d & GC_DS_TAGS) == GC_DS_PROC
              || (d & GC_DS_TAGS) == GC_DS_LENGTH);
  p = (void **)GC_MALLOC_EXPLICITLY_TYPED(large_size * sizeof(void *), d);
  TEST_ASSERT(p != NULL);
}

/* Test a very large bitmap. */
static void
test_very_large_bitmap(void)
{
  const size_t very_large_size = 10000;
  GC_descr d;
  size_t i;
  size_t bm_sz = ROUNDUP_WORDSZ(very_large_size) * sizeof(GC_word);
  GC_word *bm = (GC_word *)GC_MALLOC_ATOMIC(bm_sz);
  void **p;

  CHECK_OUT_OF_MEMORY(bm);
  memset(bm, 0, bm_sz);

  /* Set bits at regular intervals. */
  for (i = 0; i < very_large_size; i += 100)
    GC_set_bit(bm, i);

  d = GC_make_descriptor(bm, very_large_size);
  TEST_ASSERT(d != 0);

  p = (void **)GC_MALLOC_EXPLICITLY_TYPED_IGNORE_OFF_PAGE(
      very_large_size * sizeof(void *), d);
  TEST_ASSERT(p != NULL);
}

/* Test an edge case having bitmap with all bits set. */
static void
test_all_bits_set(void)
{
  const size_t size = 5000; /*< large enough */
  size_t i;
  GC_descr d;
  GC_word *bm = GC_NEW_ATOMIC_ARRAY(GC_word, ROUNDUP_WORDSZ(size));
  void **p;

  CHECK_OUT_OF_MEMORY(bm);

  /* Set all bits. */
  for (i = 0; i < size; i++) {
    GC_set_bit(bm, i);
  }

  d = GC_make_descriptor(bm, size);
  TEST_ASSERT(d != 0);

  p = (void **)GC_MALLOC_EXPLICITLY_TYPED(size * sizeof(void *), d);
  TEST_ASSERT(p != NULL);
}

/* Test edge case having a bitmap with only last bit set. */
static void
test_last_bit_set(void)
{
  const size_t size = 8000; /*< large enough */
  GC_descr d;
  size_t bm_sz = ROUNDUP_WORDSZ(size) * sizeof(GC_word);
  GC_word *bm = (GC_word *)GC_MALLOC_ATOMIC(bm_sz);
  void **p;

  CHECK_OUT_OF_MEMORY(bm);
  memset(bm, 0, bm_sz);

  GC_set_bit(bm, size - 1);
  d = GC_make_descriptor(bm, size);
  TEST_ASSERT(d != 0);

  p = (void **)GC_MALLOC_EXPLICITLY_TYPED(size * sizeof(void *), d);
  TEST_ASSERT(p != NULL);
}

/* Test multiple descriptors to check the extended descriptor array growth. */
static void
test_multiple_descriptors(void)
{
  const size_t num_descriptors = 10;
  const size_t size = 3000; /*< large enough */
  size_t i;
  GC_descr descriptors[10];
  void **objects[10];

  for (i = 0; i < num_descriptors; i++) {
    size_t j;
    size_t bm_sz = ROUNDUP_WORDSZ(size) * sizeof(GC_word);
    GC_word *bm = (GC_word *)GC_MALLOC_ATOMIC(bm_sz);

    CHECK_OUT_OF_MEMORY(bm);
    memset(bm, 0, bm_sz);

    /* Set some pattern of bits. */
    for (j = 0; j < size; j += (i + 1) * 10)
      GC_set_bit(bm, j);

    descriptors[i] = GC_make_descriptor(bm, size);
    TEST_ASSERT(descriptors[i] != 0);
    TEST_ASSERT((descriptors[i] & GC_DS_TAGS) == GC_DS_PROC
                || (descriptors[i] & GC_DS_TAGS) == GC_DS_LENGTH);

    objects[i] = (void **)GC_MALLOC_EXPLICITLY_TYPED(size * sizeof(void *),
                                                     descriptors[i]);
    TEST_ASSERT(objects[i] != NULL);
  }
}

/* Test array allocation with typed descriptors. */
static void
test_typed_array_allocation(void)
{
  const size_t nelements = 100;
  const size_t element_size = 2500; /*< large enough */
  GC_descr d;
  size_t bm_sz = ROUNDUP_WORDSZ(element_size) * sizeof(GC_word);
  GC_word *bm = (GC_word *)GC_MALLOC_ATOMIC(bm_sz);
  void **p;

  CHECK_OUT_OF_MEMORY(bm);
  memset(bm, 0, bm_sz);

  GC_set_bit(bm, 0);
  GC_set_bit(bm, 25);
  GC_set_bit(bm, 49);
  GC_set_bit(bm, 100);
  GC_set_bit(bm, 250);
  GC_set_bit(bm, 499);
  GC_set_bit(bm, 1000);
  GC_set_bit(bm, 2499);

  d = GC_make_descriptor(bm, element_size);
  TEST_ASSERT(d != 0);

  p = (void **)GC_CALLOC_EXPLICITLY_TYPED(nelements,
                                          element_size * sizeof(void *), d);
  TEST_ASSERT(p != NULL);
}

/* Test the scenario of memory growth and reallocation. */
static void
test_memory_growth(void)
{
  const size_t initial_size = 2000; /*< large enough */
  const size_t growth_factor = 50;
  size_t i;
  void ***objects;
  GC_descr *descriptors = GC_NEW_ARRAY(GC_descr, 50);

  CHECK_OUT_OF_MEMORY(descriptors);
  objects = GC_NEW_ARRAY(void **, 50);
  CHECK_OUT_OF_MEMORY(objects);

  /* Create progressively larger descriptors. */
  for (i = 0; i < 50; i++) {
    size_t size = initial_size + i * growth_factor;
    size_t j;
    size_t bm_sz = ROUNDUP_WORDSZ(size) * sizeof(GC_word);
    GC_word *bm = (GC_word *)GC_MALLOC_ATOMIC(bm_sz);

    CHECK_OUT_OF_MEMORY(bm);
    memset(bm, 0, bm_sz);

    /* Set bits in a pattern. */
    for (j = 0; j < size; j += 25 + i)
      GC_set_bit(bm, j);

    descriptors[i] = GC_make_descriptor(bm, size);
    TEST_ASSERT(descriptors[i] != 0);
    objects[i] = (void **)GC_MALLOC_EXPLICITLY_TYPED(size * sizeof(void *),
                                                     descriptors[i]);
    TEST_ASSERT(objects[i] != NULL);
  }
}

/* Test some error conditions and edge cases. */
static void
test_edge_cases(void)
{
  const size_t word_bits = GC_WORDSZ;
  GC_word *bm = GC_NEW_ATOMIC(GC_word);

  CHECK_OUT_OF_MEMORY(bm);
  bm[0] = 1; /*< set the first bit */
  TEST_ASSERT(GC_make_descriptor(bm, 1) != 0);

  /* Test with the size equal to machine word size. */
  bm = GC_NEW_ATOMIC(GC_word);
  CHECK_OUT_OF_MEMORY(bm);
  bm[0] = ~(GC_word)0; /*< set all bits */
  TEST_ASSERT(GC_make_descriptor(bm, word_bits) != 0);
}

/* Test that the allocated objects are properly marked. */
static void
test_gc_collection(void)
{
  const size_t size = 4000; /*< large enough */
  GC_descr d;
  void **obj1, **obj2;
  void *ptr1, *ptr2;
  size_t bm_sz = ROUNDUP_WORDSZ(size) * sizeof(GC_word);
  GC_word *bm = (GC_word *)GC_MALLOC_ATOMIC(bm_sz);

  CHECK_OUT_OF_MEMORY(bm);
  memset(bm, 0, bm_sz);

  /* Set some pointer fields. */
  GC_set_bit(bm, 0);
  GC_set_bit(bm, 100);
  GC_set_bit(bm, 200);
  GC_set_bit(bm, 1499);
  GC_set_bit(bm, 3999);

  d = GC_make_descriptor(bm, size);
  TEST_ASSERT(d != 0);

  obj1 = (void **)GC_MALLOC_EXPLICITLY_TYPED(size * sizeof(void *), d);
  TEST_ASSERT(obj1 != NULL);
  obj2 = (void **)GC_MALLOC_EXPLICITLY_TYPED(size * sizeof(void *), d);
  TEST_ASSERT(obj2 != NULL);

  /* Allocate some pointed-to objects. */
  ptr1 = GC_MALLOC(100);
  TEST_ASSERT(ptr1 != NULL);
  ptr2 = GC_MALLOC(200);
  TEST_ASSERT(ptr2 != NULL);

  /* Set some pointers in the typed objects. */
  obj1[0] = ptr1;
  obj1[100] = ptr2;
  obj2[200] = ptr1;
  obj2[3999] = ptr2;

  GC_gcollect();

  /* The pointed-to objects should still be alive */
  TEST_ASSERT(GC_is_heap_ptr(ptr1));
  TEST_ASSERT(GC_is_heap_ptr(ptr2));
}

#endif /* !NO_TYPED_TEST */

int
main(void)
{
#ifdef NO_TYPED_TEST
  printf("test skipped\n");
#else
  GC_INIT();
  if (GC_get_find_leak())
    printf("This test program is not designed for leak detection mode\n");
#  ifndef NO_INCREMENTAL
  GC_enable_incremental();
#  endif

  /* Tests for `GC_add_ext_descriptor()`. */
  test_small_bitmap();
  test_large_bitmap();
  test_very_large_bitmap();
  test_all_bits_set();
  test_last_bit_set();
  test_multiple_descriptors();
  test_typed_array_allocation();
  test_memory_growth();
  test_edge_cases();
  test_gc_collection();
#  if defined(ESCARGOT_USE_32BIT_IN_64BIT)
  test_compressed_range();
  test_compressed_bitmap();
  test_compressed_weak_link();
#    ifdef ENABLE_DISCLAIM
  test_finalized_payloads();
#    endif
#  endif

  printf("SUCCEEDED\n");
#endif
  return 0;
}
