/*
 * Copyright (c) 2015-present Samsung Electronics Co., Ltd
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
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

#include "Allocator.h"

#ifdef PROFILE_MASSIF

#include <cstring>
#include <unordered_map>
#include <vector>

std::unordered_map<void*, void*> g_addressTable;
std::vector<void*> g_freeList;

void unregisterGCAddress(void* address)
{
    assert(g_addressTable.find(address) != g_addressTable.end());
    if (g_addressTable.find(address) != g_addressTable.end()) {
        auto iter = g_addressTable.find(address);
        free(iter->second);
        g_addressTable.erase(iter);
    }
}

void registerGCAddress(void* address, size_t siz)
{
    if (g_addressTable.find(address) != g_addressTable.end()) {
        unregisterGCAddress(address);
    }
    g_addressTable[address] = malloc(siz);
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

void* GC_strdup_hook(const char* str) {
#if defined(GC_DEBUG)
    void* ptr = GC_debug_strdup(str, GC_EXTRAS);
#else
    void* ptr = GC_strdup(str);
#endif
    registerGCAddress(ptr, std::strlen(str) + 1);
    return ptr;
}

void* GC_strndup_hook(const char* str) {
    size_t len = std::strlen(str);
#if defined(GC_DEBUG)
    void* ptr = GC_debug_strndup(str, len, GC_EXTRAS);
#else
    void* ptr = GC_strndup(str, len);
#endif
    registerGCAddress(ptr, len + 1);
    return ptr;
}

void* GC_realloc_hook(void* address, size_t siz) {
#if defined(GC_DEBUG)
    void* ptr = GC_debug_realloc(address, siz, GC_EXTRAS);
#else
    void* ptr = GC_realloc(address, siz);
#endif
    unregisterGCAddress(address);
    registerGCAddress(ptr, siz);
    return ptr;
}

void GC_free_hook(void* address)
{
#if defined(GC_DEBUG)
    GC_debug_free(address);
#else
    GC_free(address);
#endif
    unregisterGCAddress(address);
}

#endif
