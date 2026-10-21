/* Copyright (c) 2026 Samsung Electronics Co., Ltd. All rights reserved. */
#include "gc/gc.h"
#include "gc/gc_typed.h"
#include <cstdlib>
#include <sys/mman.h>
#include <utility>
#include <vector>

static std::vector<std::pair<void *, size_t>> mappings;

extern "C" __attribute__((visibility("default"))) unsigned
runtime_unload_initialize(int incremental)
{
  GC_set_on_os_get_mem([](void *ptr, size_t bytes) {
    mappings.push_back(std::make_pair(ptr, bytes));
  });
  if (incremental)
    setenv("GC_ENABLE_INCREMENTAL", "1", 1);
  else
    unsetenv("GC_ENABLE_INCREMENTAL");
  GC_init();
  GC_word bitmap = 1;
  GC_descr descriptor = GC_make_descriptor(&bitmap, 2);
  void **object = static_cast<void **>(
      GC_malloc_explicitly_typed(2 * sizeof(void *), descriptor));
  object[0] = GC_malloc(4096);
  static_cast<char *>(object[0])[0] = 42;
  GC_gcollect();
  if (static_cast<char *>(object[0])[0] != 42)
    std::abort();
  return GC_get_actual_vdb();
}

extern "C" __attribute__((visibility("default"))) void
runtime_unload_destroy(void)
{
  GC_set_on_os_get_mem(nullptr);
  GC_destroy_runtime_for_shared_library_unload();
  for (const auto &mapping : mappings)
    munmap(mapping.first, mapping.second);
  mappings.clear();
}
