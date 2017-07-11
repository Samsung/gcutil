/*
 * Copyright (c) 2016-present Samsung Electronics Co., Ltd
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "Allocator.h"

#ifdef PROFILE_MASSIF

#include <unordered_map>
#include <vector>

std::unordered_map<void*, void*> g_addressTable;
std::vector<void*> g_freeList;

void unregisterGCAddress(void* address)
{
    // ASSERT(g_addressTable.find(address) != g_addressTable.end());
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
    void* ptr = GC_malloc(siz);
    registerGCAddress(ptr, siz);
    return ptr;
}
void* GC_malloc_atomic_hook(size_t siz)
{
    void* ptr = GC_malloc_atomic(siz);
    registerGCAddress(ptr, siz);
    return ptr;
}

void* GC_generic_malloc_hook(size_t siz, int kind)
{
    void* ptr = GC_generic_malloc(siz, kind);
    registerGCAddress(ptr, siz);
    return ptr;
}

void* GC_generic_malloc_ignore_off_page_hook(size_t siz, int kind)
{
    void* ptr = GC_generic_malloc_ignore_off_page(siz, kind);
    registerGCAddress(ptr, siz);
    return ptr;
}

void* GC_malloc_ignore_off_page_hook(size_t siz)
{
    void* ptr = GC_malloc_ignore_off_page(siz);
    registerGCAddress(ptr, siz);
    return ptr;
}

void* GC_malloc_atomic_ignore_off_page_hook(size_t siz)
{
    void* ptr = GC_malloc_atomic_ignore_off_page(siz);
    registerGCAddress(ptr, siz);
    return ptr;
}

void GC_free_hook(void* address)
{
    GC_free(address);
    unregisterGCAddress(address);
}

#endif

