PROJECT(gc)
CMAKE_MINIMUM_REQUIRED(VERSION 2.8.12 FATAL_ERROR)

INCLUDE_DIRECTORIES(include)

# Opt-in NOSYS/bare-metal (no mmap, no dlopen, no pthreads, no signals) build
# mode -- for RTOS/bare-metal ports (see Escargot's own
# docs/porting/RTOS_PORTING_GUIDE.md) that add_subdirectory() this project
# directly instead of hand-copying its source list/defines. Defaults OFF, so
# every hosted platform (Linux/Darwin/Windows/Android/Tizen) is unaffected.
OPTION(GCUTIL_NOSYS_BAREMETAL "Build BDWGC for a NOSYS/bare-metal target (no OS)" OFF)

IF (GCUTIL_NOSYS_BAREMETAL)
    # Curated source-file subset: everything else at this top level is
    # hosted-OS-only (pthread/dlopen/mmap-based) and must not be compiled for
    # a NOSYS target -- see include/private/gcconfig.h's NOSYS branch for what
    # BDWGC itself expects here. (obj_map.c does NOT belong here -- it was
    # removed upstream, commit e849d641 "Remove obj_map.c file (refactoring)";
    # an earlier hand-copied version of this list carried it as a stale,
    # silently-empty FILE(GLOB) entry.)
    SET (GCUTIL_BDWGC_SRC
        allchblk.c
        alloc.c
        backgraph.c
        blacklst.c
        checksums.c
        dbg_mlc.c
        dyn_load.c
        finalize.c
        fnlz_mlc.c
        headers.c
        mach_dep.c
        malloc.c
        mallocx.c
        mark.c
        mark_rts.c
        misc.c
        new_hblk.c
        os_dep.c
        ptr_chck.c
        reclaim.c
        typd_mlc.c
    )
    # gc_cpp.cpp deliberately excluded: its only content is the *global*
    # ::operator new/delete override, which Escargot doesn't need (its own
    # GC-managed classes derive from the `GC` base class in gc_cpp.h, whose
    # own operator new/delete overloads are all inline there) -- and
    # including it conflicts with NuttX's libsupc++.a (multiple definition
    # of `operator delete(void*)` at final link).
    SET (GCUTIL_SRC Allocator.cpp LeakChecker.cpp gc_badalc.cpp)
ELSE()
    FILE(GLOB GCUTIL_BDWGC_SRC *.c)
    SET (GCUTIL_SRC Allocator.cpp LeakChecker.cpp)
ENDIF()

SET (COMPILER_CLANG_CL OFF)
IF (${CMAKE_CXX_COMPILER_ID} STREQUAL "Clang")
    IF (DEFINED CMAKE_CXX_COMPILER_FRONTEND_VARIANT)
        IF ("${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}" STREQUAL "MSVC")
            SET (COMPILER_CLANG_CL ON)
        ENDIF()
    ENDIF()
ENDIF()

IF(${CMAKE_CXX_COMPILER_ID} STREQUAL "MSVC" OR ${COMPILER_CLANG_CL})
    SET (GCUTIL_CFLAGS_INTERNAL -DESCARGOT -DGC_DLL=1 -DGC_API=extern -DGC_API_PRIV=extern /wd4996)
    SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DGC_NO_THREADS_DISCOVERY=1 -DGC_VERSION_MAJOR=8 -DGC_VERSION_MICRO=0 -DGC_VERSION_MINOR=3)
    SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DENABLE_DISCLAIM=1 -DGC_ATOMIC_UNCOLLECTABLE=1 -DGC_DONT_REGISTER_MAIN_STATIC_DATA=1 -DGC_ENABLE_SUSPEND_THREAD=1)
    SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DHAVE_STDLIB_H -DHAVE_STRINGS_H -DHAVE_STRING_H -DLARGE_CONFIG -DUSE_WINALLOC=1 -DIGNORE_DYNAMIC_LOADING=1 -DJAVA_FINALIZATION=1 -DNO_EXECUTE_PERMISSION)
    SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DNO_WARN_HEAP_GROW_WHEN_GC_DISABLED=1)
    IF (GCUTIL_MODE STREQUAL "debug")
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DDBG_HDRS_ALL=1 -DGC_DEBUG)
    ELSE()
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DNO_DEBUGGING=1 -DNO_ALL_INTERIOR_POINTERS=1 /O2 /Oy-)
    ENDIF()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /std:c++17 /EHs")
ELSEIF (${CMAKE_CXX_COMPILER_ID} MATCHES "GNU|Clang|AppleClang") #include GNU, Clang and AppleClang all
    IF (GCUTIL_NOSYS_BAREMETAL)
        # NOSYS/bare-metal target: no mmap, no dlopen, no pthreads, no signal
        # handling, no libc heap/env/clock. Deliberately does NOT share the
        # hosted branch below -- HAVE_DLADDR/USE_MMAP/HAVE_PTHREAD_GETATTR_NP
        # and friends are all meaningless (or actively wrong) here -- and does
        # not distinguish GCUTIL_MODE debug/release; callers set their own
        # optimization level externally (e.g. via ESCARGOT_MODE/CMAKE_*_FLAGS).
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -fdata-sections -ffunction-sections -DESCARGOT -fno-strict-aliasing -Wno-unused-variable)
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DENABLE_DISCLAIM -DGC_ATOMIC_UNCOLLECTABLE)
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DGC_VERSION_MAJOR=8 -DGC_VERSION_MICRO=0 -DGC_VERSION_MINOR=3)
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL}
            -DNOSYS
            -DSMALL_CONFIG
            -DGC_NOT_DLL
            -DGC_NO_SIGHANDLER
            -DNO_GETENV
            -DNO_CLOCK
            -DGC_NEW_ABORTS_ON_OOM
            -DGC_DISABLE_INCREMENTAL
            -DIGNORE_DYNAMIC_LOADING
            -DDONT_USE_MMAP
            -DGC_NO_DLOPEN
        )
        # Caller-supplied initial BDWGC heap size (differs per port -- e.g.
        # the FreeRTOS sample uses 4 MB, the NuttX sample uses 1 MB, to size
        # around each target's own simulated/emulated execution speed).
        IF (NOT DEFINED GCUTIL_INITIAL_HEAP_SIZE)
            SET (GCUTIL_INITIAL_HEAP_SIZE "(1*1024*1024)")
        ENDIF()
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} "-DGC_INITIAL_HEAP_SIZE=${GCUTIL_INITIAL_HEAP_SIZE}")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -fno-rtti")
    ELSE()
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -g3 -fdata-sections -ffunction-sections -DESCARGOT -fno-strict-aliasing -DGC_DLL=1 -fvisibility=hidden -Wno-unused-variable)
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DENABLE_DISCLAIM=1 -DGC_ATOMIC_UNCOLLECTABLE=1 -DGC_DONT_REGISTER_MAIN_STATIC_DATA=1 -DGC_ENABLE_SUSPEND_THREAD=1 -DGC_BUILD=1 -DGC_VISIBILITY_HIDDEN_SET=1)
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DGC_NO_THREADS_DISCOVERY=1 -DGC_VERSION_MAJOR=8 -DGC_VERSION_MICRO=0 -DGC_VERSION_MINOR=3)
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DHAVE_DLADDR=1 -DHAVE_DLFCN_H=1 -DHAVE_DL_ITERATE_PHDR=1 -DHAVE_INTTYPES_H=1 -DHAVE_MEMORY_H=1 -DHAVE_STDINT_H=1 -DHAVE_STDLIB_H=1 -DHAVE_STRINGS_H=1)
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DHAVE_STRING_H=1 -DHAVE_SYS_STAT_H=1 -DHAVE_SYS_TYPES_H=1 -DHAVE_UNISTD_H=1)
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DIGNORE_DYNAMIC_LOADING=1 -DJAVA_FINALIZATION=1 -DMUNMAP_THRESHOLD=1 -DNO_EXECUTE_PERMISSION=1 -DSTDC_HEADERS=1 -DUSE_MMAP=1 -DUSE_MUNMAP=1)
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DNO_WARN_HEAP_GROW_WHEN_GC_DISABLED=1 -DDONT_PROTECT_PTRFREE=1)
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DHAVE_PTHREAD_GETATTR_NP=1 -DUSE_GET_STACKBASE_FOR_MAIN=1)
        IF (GCUTIL_MODE STREQUAL "debug")
            SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DKEEP_BACK_PTRS=1 -DSAVE_CALL_COUNT=8 -DDBG_HDRS_ALL=1 -DGC_DEBUG -O0)
        ELSE()
            SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DNO_DEBUGGING=1 -DNO_ALL_INTERIOR_POINTERS=1 -O2)
        ENDIF()
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -fno-rtti")
    ENDIF()
ELSE()
    MESSAGE (FATAL_ERROR "unsupported target")
ENDIF()

IF (GCUTIL_ENABLE_THREADING)
    SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -D_REENTRANT=1 -DGC_THREAD_ISOLATE=1)
    IF (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Do not define NO_MPROTECT_VDB/NO_UFFDWP_VDB: GC_dirty_init()
        # (os_dep.c) tries uffd first and falls back to mprotect at
        # runtime on its own. uffd works in a plain shell/root process,
        # but the Tizen app sandbox blocks it; forcing uffd-only here
        # would leave incremental GC silently disabled in that sandbox
        # instead of falling back.
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DNO_GWW_VDB)
    ENDIF()
    IF (GCUTIL_ENABLE_TLS_ACCESS_BY_ADDRESS)
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DENABLE_TLS_ACCESS_BY_ADDRESS=1)
    ENDIF()
    IF (GCUTIL_ENABLE_TLS_ACCESS_BY_PTHREAD_KEY)
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DENABLE_TLS_ACCESS_BY_PTHREAD_KEY=1)
    ENDIF()
ENDIF()

add_compile_options(${GCUTIL_CFLAGS_INTERNAL})
add_compile_options(${GCUTIL_CFLAGS})
SET (GCUTIL_CFLAGS_FROM_ENV $ENV{CFLAGS})
SEPARATE_ARGUMENTS(GCUTIL_CFLAGS_FROM_ENV)
# add ${GCUTIL_CFLAGS_FROM_EXTERNAL} at the end
add_compile_options(${GCUTIL_CFLAGS_FROM_ENV} ${GCUTIL_CFLAGS_FROM_EXTERNAL})

ADD_LIBRARY(gc-lib STATIC ${GCUTIL_BDWGC_SRC} ${GCUTIL_SRC})
# ADD_LIBRARY(gc-lib-shared SHARED ${GCUTIL_BDWGC_SRC} ${GCUTIL_SRC})

target_include_directories(gc-lib PUBLIC ./include)
