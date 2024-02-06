/*
 * Copyright (c) 2015-present Samsung Electronics Co., Ltd
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
 *  USA
*/

#include "GCUtilInternal.h"
#include "Allocator.h"

#include <vector>
#include <algorithm>

#ifdef ESCARGOT_MEM_STATS
#include <cstring>
#include <map>
#ifdef ESCARGOT_VALGRIND
#include <valgrind/valgrind.h>
#endif

struct AllocInfo {
    GC_finalization_proc user_cb;
    void* user_data;
    size_t size;
};

struct HeapInfo {
    size_t allocated;
    size_t peak_allocated;
    size_t total_allocated;
    size_t peak_waste;
    size_t total_waste;
    size_t alloc_count;
    size_t free_count;
};

// This structure holds information about the size of the
// allcoated memory area and a finalization user callback
// with its user defined data.
std::map<void*, AllocInfo>& addressTable()
{
    static MAY_THREAD_LOCAL std::map<void*, AllocInfo> table;
    return table;
}

static MAY_THREAD_LOCAL HeapInfo heapInfo = { 0, 0, 0, 0, 0, 0, 0 };

// The addressTable allocation should be in a separated function. This is
// important, because the noise (helper structiore allcoations) can be
// filtered out by the Freya tool of Valgrind.
void createAddressEntry(void* address, size_t size)
{
    auto it = addressTable().find(address);
    // The address should not exist.
    assert(it == addressTable().end());

    addressTable()[address] = { nullptr, nullptr, size };
}

void unregisterGCAddress(void* address, void* data)
{
    auto it = addressTable().find(address);
    // The address should exist.
    if (it == addressTable().end()) {
        return;
    }

    AllocInfo allocInfo = it->second;
    // Execute the user defined callback.
    if (allocInfo.user_cb)
        allocInfo.user_cb(address, allocInfo.user_data);

    heapInfo.allocated -= allocInfo.size;
    heapInfo.free_count++;

    addressTable().erase(it);

#ifdef ESCARGOT_VALGRIND
    VALGRIND_FREELIKE_BLOCK(address, 0);
#endif
    // Unregister the callback function. This is necessary if an address have a
    // registered finalizer function, but the memory area is explicitly deallocated
    // by GC_FREE.
#if defined(GC_DEBUG)
    GC_debug_register_finalizer_no_order(address, nullptr, nullptr, nullptr, nullptr);
#else
    GC_register_finalizer_no_order(address, nullptr, nullptr, nullptr, nullptr);
#endif
}

void registerGCAddress(void* address, size_t siz)
{
    auto it = addressTable().find(address);
    // The address should not exist.
    assert(it == addressTable().end());

    createAddressEntry(address, siz);

#ifdef ESCARGOT_VALGRIND
    VALGRIND_MALLOCLIKE_BLOCK(address, siz, 0, 0);
#endif
    // Calculate statistics.
    size_t waste = GC_size(address) - siz;

    heapInfo.total_waste += waste;
    heapInfo.allocated += siz;
    heapInfo.total_allocated += siz;
    heapInfo.alloc_count++;

    if (waste > heapInfo.peak_waste)
        heapInfo.peak_waste = waste;

    if (heapInfo.allocated > heapInfo.peak_allocated)
        heapInfo.peak_allocated = heapInfo.allocated;

    // Register finalizer callback for all allocated memory addresses.
    // In this case a notification happens before deallocating the
    // memory area by the GC.
#if defined(GC_DEBUG)
    GC_debug_register_finalizer_no_order(address, unregisterGCAddress, nullptr, nullptr, nullptr);
#else
    GC_register_finalizer_no_order(address, unregisterGCAddress, nullptr, nullptr, nullptr);
#endif
}

void GC_print_heap_usage()
{
    printf("Heap stats (BDWGC):\n");
    printf("  Total allocated: %zu bytes\n", heapInfo.total_allocated);
    printf("  Peak allocated: %zu bytes\n", heapInfo.peak_allocated);
    printf("  Total Waste: %zu bytes\n", heapInfo.total_waste);
    printf("  Peak waste: %zu bytes\n", heapInfo.peak_waste);
    printf("  Leak: %zu bytes\n", heapInfo.allocated);
    printf("  Allocation count: %zu\n", heapInfo.alloc_count);
    printf("  Free count: %zu\n", heapInfo.free_count);
}

// This function saves the used defined callback and data for the pointer.
// There are GC_REGISTER_FINALIZER_NO_ORDER usages within Escargot (e.g.
// in the ByteCode.h file).
void GC_register_finalizer_no_order_hook(void* obj, GC_finalization_proc fn,
                                         void* cd, GC_finalization_proc *ofn,
                                         void** ocd)
{
    auto it = addressTable().find(obj);
    // The address should exist.
    assert(it != addressTable().end());

    (it->second).user_cb = fn;
    (it->second).user_data = cd;
}

void* GC_malloc_hook(size_t siz)
{
#if defined(GC_DEBUG)
    void* ptr = GC_debug_malloc(siz, GC_EXTRAS);
#else
    void* ptr = GC_malloc(siz);
#endif
    registerGCAddress(ptr, siz);
    return ptr;
}

void* GC_malloc_uncollectable_hook(size_t siz)
{
#if defined(GC_DEBUG)
    void* ptr = GC_debug_malloc_uncollectable(siz, GC_EXTRAS);
#else
    void* ptr = GC_malloc_uncollectable(siz);
#endif
    registerGCAddress(ptr, siz);
    return ptr;
}

void* GC_malloc_atomic_hook(size_t siz)
{
#if defined(GC_DEBUG)
    void* ptr = GC_debug_malloc_atomic(siz, GC_EXTRAS);
#else
    void* ptr = GC_malloc_atomic(siz);
#endif
    registerGCAddress(ptr, siz);
    return ptr;
}

void* GC_malloc_atomic_uncollectable_hook(size_t siz)
{
#if defined(GC_DEBUG)
    void* ptr = GC_debug_malloc_atomic_uncollectable(siz, GC_EXTRAS);
#else
    void* ptr = GC_malloc_atomic_uncollectable(siz);
#endif
    registerGCAddress(ptr, siz);
    return ptr;
}

void* GC_malloc_explicitly_typed_hook(size_t siz, GC_descr desc)
{
#if defined(GC_DEBUG)
    void* ptr = GC_debug_malloc(siz, GC_EXTRAS);
#else
    void* ptr = GC_malloc_explicitly_typed(siz, desc);
#endif
    registerGCAddress(ptr, siz);
    return ptr;
}

void* GC_generic_malloc_hook(size_t siz, int kind)
{
#if defined(GC_DEBUG)
    void* ptr = GC_debug_generic_malloc(siz, kind, GC_EXTRAS);
#else
    void* ptr = GC_generic_malloc(siz, kind);
#endif
    registerGCAddress(ptr, siz);
    return ptr;
}

void* GC_malloc_stubborn_hook(size_t siz)
{
#if defined(GC_DEBUG)
    void* ptr = GC_debug_malloc_stubborn(siz, GC_EXTRAS);
#else
    void* ptr = GC_malloc_stubborn(siz);
#endif
    registerGCAddress(ptr, siz);
    return ptr;
}

void* GC_strdup_hook(const char* str)
{
#if defined(GC_DEBUG)
    void* ptr = GC_debug_strdup(str, GC_EXTRAS);
#else
    void* ptr = GC_strdup(str);
#endif
    registerGCAddress(ptr, std::strlen(str) + 1);
    return ptr;
}

void* GC_strndup_hook(const char* str)
{
    size_t len = std::strlen(str);
#if defined(GC_DEBUG)
    void* ptr = GC_debug_strndup(str, len, GC_EXTRAS);
#else
    void* ptr = GC_strndup(str, len);
#endif
    registerGCAddress(ptr, len + 1);
    return ptr;
}

void* GC_realloc_hook(void* address, size_t siz)
{
    if (address) {
        unregisterGCAddress(address, nullptr);
    }
#if defined(GC_DEBUG)
    void* ptr = GC_debug_realloc(address, siz, GC_EXTRAS);
#else
    void* ptr = GC_realloc(address, siz);
#endif
    if (ptr) {
        registerGCAddress(ptr, siz);
    }
    return ptr;
}

void GC_free_hook(void* address)
{
    if (!address) {
        return;
    }
    unregisterGCAddress(address, nullptr);
#if defined(GC_DEBUG)
    GC_debug_free(address);
#else
    GC_free(address);
#endif
}

#endif
