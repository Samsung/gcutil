PROJECT(gc)
# The "...4.0" range keeps this buildable on CMake 4.x, which hard-errors
# when the effective policy version is below 3.5; pre-3.12 CMake doesn't
# understand the range syntax and just uses 2.8.12 as before (see
# `cmake --help-command cmake_minimum_required`).
CMAKE_MINIMUM_REQUIRED(VERSION 2.8.12...4.0 FATAL_ERROR)

# Set default build type if not specified (standard CMake practice)
IF (CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    IF (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        SET (CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
        SET_PROPERTY(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
    ENDIF()
ENDIF()

# Determine optimization flags for release-type builds (Release, RelWithDebInfo, MinSizeRel).
# Compute both variants unconditionally (instead of branching on CMAKE_BUILD_TYPE here) so
# the $<CONFIG:MinSizeRel> generator expression below picks the right one at build time even
# for multi-config generators, where CMAKE_BUILD_TYPE isn't set at configure time.
IF (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    SET (GCUTIL_RELEASE_FLAGS_MINSIZEREL "-Os")
    SET (GCUTIL_RELEASE_FLAGS_OTHER "-O2")
ELSEIF (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR COMPILER_CLANG_CL)
    SET (GCUTIL_RELEASE_FLAGS_MINSIZEREL "/O1")
    SET (GCUTIL_RELEASE_FLAGS_OTHER "/O2")
ENDIF()

# Include CMake module for compile test
INCLUDE(CheckCSourceCompiles)

# Auto-detect bare-metal/RTOS targets (see docs/porting/RTOS_PORTING_GUIDE.md)
IF (CMAKE_SYSTEM_NAME STREQUAL "Generic" OR
    CMAKE_SYSTEM_NAME STREQUAL "BareMetal" OR
    CMAKE_SYSTEM_NAME STREQUAL "FreeRTOS" OR
    CMAKE_SYSTEM_NAME STREQUAL "NuttX" OR
    CMAKE_SYSTEM_NAME STREQUAL "Zephyr" OR
    CMAKE_SYSTEM_NAME STREQUAL "RIOT" OR
    CMAKE_SYSTEM_NAME STREQUAL "MbedOS" OR
    CMAKE_SYSTEM_NAME STREQUAL "Mynewt" OR
    CMAKE_SYSTEM_NAME STREQUAL "Emscripten" OR
    CMAKE_SYSTEM_NAME STREQUAL "WASI" OR
    CMAKE_C_COMPILER_TARGET MATCHES "-none-eabi$" OR
    CMAKE_C_COMPILER_TARGET MATCHES "-elf$")
    SET(GCUTIL_NOSYS_BAREMETAL ON)
ELSE()
    SET(GCUTIL_NOSYS_BAREMETAL OFF)
ENDIF()

# Source file list. Kept as an explicit, hand-curated list for the
# NOSYS_BAREMETAL case (RTOS/bare-metal has no pthreads/dlopen/mmap, so most
# hosted-OS .c files must not be compiled at all -- see
# docs/porting/RTOS_PORTING_GUIDE.md and gcconfig.h's NOSYS branch). For every
# hosted platform (Linux/Darwin/Windows/Android/Tizen), FILE(GLOB) is used
# instead of hand-listing every file: this project's bdwgc vendor drop compiles
# EVERY *.c here on EVERY hosted platform unconditionally (win32_threads.c,
# pthread_support.c/pthread_stop_world.c, darwin_stop_world.c, dyn_load.c,
# etc. all internally #ifdef-guard themselves down to a no-op object on the
# platforms that don't need them -- that's how upstream bdwgc's own build
# systems consume this same source tree). A prior revision of this file
# replaced the glob with a hand-curated list (to avoid GLOB's well-known
# "doesn't retrigger reconfigure when a file is added" caveat) and silently
# dropped dyn_load.c/backgraph.c/checksums.c/gc_dlopen.c/gcj_mlc.c/
# pthread_start.c/specific.c/thread_local_alloc.c/darwin_stop_world.c in the
# process -- dyn_load.c's absence is what breaks the Windows link with
# "unresolved external symbol GC_register_dynamic_libraries" (mark_rts.c calls
# it unconditionally whenever ANY_MSWIN is defined, regardless of the
# DYNAMIC_LOADING macro dyn_load.c's own definition is nominally gated behind
# -- see dyn_load.c's `#ifdef ANY_MSWIN` block). Do not go back to a
# hand-curated list here without adding a build step that fails loudly if it
# ever drifts from `ls *.c` again.
IF (GCUTIL_NOSYS_BAREMETAL)
    # Curated bare-metal subset: everything else at this level is
    # hosted-OS-only (pthread/dlopen/mmap-based) and must not be compiled for
    # a NOSYS target -- see gcconfig.h's NOSYS branch for what BDWGC itself
    # expects here.
    SET (GCUTIL_BDWGC_SRC
        allchblk.c alloc.c backgraph.c blacklst.c checksums.c dbg_mlc.c dyn_load.c
        finalize.c fnlz_mlc.c headers.c mach_dep.c malloc.c mallocx.c mark.c mark_rts.c
        misc.c new_hblk.c os_dep.c ptr_chck.c reclaim.c typd_mlc.c
    )
    # gc_badalc.cpp provides the global ::operator new/delete override used by
    # bare-metal ports that don't otherwise get one from a hosted C++ runtime.
    SET (GCUTIL_SRC Allocator.cpp LeakChecker.cpp gc_badalc.cpp)
ELSE()
    FILE(GLOB GCUTIL_BDWGC_SRC ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
    SET (GCUTIL_SRC Allocator.cpp LeakChecker.cpp)
ENDIF()

# FLAGS FOR COMPILER
IF (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR COMPILER_CLANG_CL)
    # NOTE: this branch used to be reduced to just "/wd4146 /DGC_NOT_DLL" during
    # the CMake-conventions refactor, silently dropping every define below --
    # including -DENABLE_DISCLAIM=1/-DGC_ATOMIC_UNCOLLECTABLE=1, which gate
    # bdwgc's GC_register_disclaim_proc/GC_finalized_malloc/
    # GC_malloc_atomic_uncollectable/etc (finalize.c). Without them, escargot.lib
    # fails to link on Windows with "unresolved external symbol
    # GC_finalized_malloc" and friends (confirmed via CI). Restored to match
    # this project's pre-refactor MSVC flags 1:1, just re-homed into
    # GCUTIL_CFLAGS_INTERNAL (target-scoped via TARGET_COMPILE_OPTIONS below,
    # instead of a bare SET(CMAKE_CXX_FLAGS ...) global mutation) and with the
    # debug/release macro split converted to the same $<CONFIG:...> generator-
    # expression pattern used by the GNU/Clang/AppleClang branch below.
    SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DESCARGOT -DGC_DLL=1 -DGC_API=extern -DGC_API_PRIV=extern /wd4996 /wd4146)
    SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DGC_NO_THREADS_DISCOVERY=1 -DGC_VERSION_MAJOR=8 -DGC_VERSION_MICRO=0 -DGC_VERSION_MINOR=3)
    SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DENABLE_DISCLAIM=1 -DGC_ATOMIC_UNCOLLECTABLE=1 -DGC_DONT_REGISTER_MAIN_STATIC_DATA=1 -DGC_ENABLE_SUSPEND_THREAD=1)
    SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DHAVE_STDLIB_H -DHAVE_STRINGS_H -DHAVE_STRING_H -DLARGE_CONFIG -DUSE_WINALLOC=1 -DIGNORE_DYNAMIC_LOADING=1 -DJAVA_FINALIZATION=1 -DNO_EXECUTE_PERMISSION)
    SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DNO_WARN_HEAP_GROW_WHEN_GC_DISABLED=1)
    SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} /std:c++17 /EHs)
    SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL}
        $<$<CONFIG:Debug>:-DDBG_HDRS_ALL=1>
        $<$<CONFIG:Debug>:-DGC_DEBUG>
        $<$<NOT:$<CONFIG:Debug>>:-DNO_DEBUGGING=1>
        $<$<NOT:$<CONFIG:Debug>>:-DNO_ALL_INTERIOR_POINTERS=1>
        $<$<NOT:$<CONFIG:Debug>>:/Oy->)
ELSEIF (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang") #include GNU, Clang and AppleClang all
    IF (GCUTIL_NOSYS_BAREMETAL)
        # GCUTIL_NOSYS_BAREMETAL mode: curated bare-metal subset (no pthreads/mmap)
        # matches gcconfig.h's ARM32/NOSYS branch. GCUTIL_BDWGC_SRC/GCUTIL_SRC
        # are already set above.
        # -DNOSYS selects non-pthread branches; other flags prevent standard OS dependency
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -fdata-sections -ffunction-sections -DESCARGOT -fno-strict-aliasing -DGC_DLL=1 -fvisibility=hidden -Wno-unused-variable)
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DENABLE_DISCLAIM=1 -DGC_ATOMIC_UNCOLLECTABLE=1 -DGC_DONT_REGISTER_MAIN_STATIC_DATA=1 -DGC_BUILD=1 -DGC_VISIBILITY_HIDDEN_SET=1)
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DGC_NO_THREADS_DISCOVERY=1 -DGC_VERSION_MAJOR=8 -DGC_VERSION_MICRO=0 -DGC_VERSION_MINOR=3)
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DHAVE_INTTYPES_H=1 -DHAVE_MEMORY_H=1 -DHAVE_STDINT_H=1 -DHAVE_STDLIB_H=1 -DHAVE_STRINGS_H=1)
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DHAVE_STRING_H=1 -DHAVE_SYS_STAT_H=1 -DHAVE_SYS_TYPES_H=1 -DHAVE_UNISTD_H=1)
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DIGNORE_DYNAMIC_LOADING=1 -DJAVA_FINALIZATION=1 -DMUNMAP_THRESHOLD=1 -DNO_EXECUTE_PERMISSION=1 -DSTDC_HEADERS=1 -DNO_MMAP=1)
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DNO_WARN_HEAP_GROW_WHEN_GC_DISABLED=1 -DDONT_PROTECT_PTRFREE=1)
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DNOSYS -DNO_PTHREAD_TRYLOCK -DNO_CLOCK)
        # SMALL_CONFIG / GC_DISABLE_INCREMENTAL: dropped during the CMake-
        # conventions refactor (present unconditionally in this branch
        # pre-refactor -- bare-metal targets are always memory-constrained,
        # independent of the ESCARGOT_SMALL_CONFIG cmake option). Both are
        # bdwgc's own macros (unrelated to ESCARGOT_SMALL_CONFIG) and both
        # directly size fields of bdwgc's single global static struct
        # (GC_arrays_instance, misc.c) -- SMALL_CONFIG shrinks HBLKSIZE/
        # MAX_ROOT_SETS/LOG_PHT_ENTRIES (gc_priv.h), GC_DISABLE_INCREMENTAL
        # compiles out the page_hash_table fields entirely. Their absence
        # was found to explain a ~100KB .bss regression in the FreeRTOS
        # sample's CI size report (confirmed via reconstructing the affected
        # struct fields' sizes from gc_priv.h -- ~106KB estimated vs the
        # ~102KB observed delta).
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DSMALL_CONFIG -DGC_DISABLE_INCREMENTAL)

        # User defined heap size for bare-metal / RTOS ports. NOTE: bdwgc's
        # own macro is GC_INITIAL_HEAP_SIZE (see misc.c's "#ifdef
        # GC_INITIAL_HEAP_SIZE") -- this used to be defined as plain
        # INITIAL_HEAP_SIZE (missing the GC_ prefix) during the CMake-
        # conventions refactor, so bdwgc never saw it and silently fell back
        # to its own built-in default instead of the requested size (caught
        # via the FreeRTOS sample's own CI size/memory report: "GC heap
        # after init" dropped from a baseline of 4096 KiB to 292 KiB with
        # this bug, even though GCUTIL_INITIAL_HEAP_SIZE was still being set
        # to (4*1024*1024) by samples/rtos/freertos/CMakeLists.txt).
        IF (NOT DEFINED GCUTIL_INITIAL_HEAP_SIZE)
            SET (GCUTIL_INITIAL_HEAP_SIZE "(1*1024*1024)")
        ENDIF()
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DGC_INITIAL_HEAP_SIZE=${GCUTIL_INITIAL_HEAP_SIZE})
        
        # skip tests for userfaultfd / thread safety on bare-metal
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DNO_UFFDWP_VDB -DNO_GWW_VDB)
        set_source_files_properties(${GCUTIL_SRC} PROPERTIES COMPILE_FLAGS "-std=gnu++11 -fno-rtti")
    ELSE()
        # Normal hosted build (assumes pthreads, mmap, etc. are available)
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -fdata-sections -ffunction-sections -DESCARGOT -fno-strict-aliasing -DGC_DLL=1 -fvisibility=hidden -Wno-unused-variable)
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DENABLE_DISCLAIM=1 -DGC_ATOMIC_UNCOLLECTABLE=1 -DGC_DONT_REGISTER_MAIN_STATIC_DATA=1 -DGC_ENABLE_SUSPEND_THREAD=1 -DGC_BUILD=1 -DGC_VISIBILITY_HIDDEN_SET=1)
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DGC_NO_THREADS_DISCOVERY=1 -DGC_VERSION_MAJOR=8 -DGC_VERSION_MICRO=0 -DGC_VERSION_MINOR=3)
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DHAVE_DLADDR=1 -DHAVE_DLFCN_H=1 -DHAVE_DL_ITERATE_PHDR=1 -DHAVE_INTTYPES_H=1 -DHAVE_MEMORY_H=1 -DHAVE_STDINT_H=1 -DHAVE_STDLIB_H=1 -DHAVE_STRINGS_H=1)
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DHAVE_STRING_H=1 -DHAVE_SYS_STAT_H=1 -DHAVE_SYS_TYPES_H=1 -DHAVE_UNISTD_H=1)
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DIGNORE_DYNAMIC_LOADING=1 -DJAVA_FINALIZATION=1 -DMUNMAP_THRESHOLD=1 -DNO_EXECUTE_PERMISSION=1 -DSTDC_HEADERS=1 -DUSE_MMAP=1 -DUSE_MUNMAP=1)
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DNO_WARN_HEAP_GROW_WHEN_GC_DISABLED=1 -DDONT_PROTECT_PTRFREE=1)
        # pthread_getattr_np() is Linux-specific. macOS uses pthread_get_stackaddr_np() instead.
        # os_dep.c has Darwin-specific handling (lines 1443-1451) which requires these macros unset.
        IF (CMAKE_SYSTEM_NAME STREQUAL "Linux")
            SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DHAVE_PTHREAD_GETATTR_NP=1 -DUSE_GET_STACKBASE_FOR_MAIN=1)
        ENDIF()
        # Multi-config support: use generator expressions unconditionally -- these
        # work correctly at generate-time for BOTH single-config (Ninja, Makefiles,
        # via CMAKE_BUILD_TYPE) and multi-config (Visual Studio, Xcode, Ninja Multi-
        # Config) generators, so there's no need to branch on
        # CMAKE_CONFIGURATION_TYPES / CMAKE_BUILD_TYPE at configure time.
        IF (CMAKE_SYSTEM_NAME STREQUAL "Android")
            SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL}
                $<$<CONFIG:Debug>:-DGC_DEBUG>
                $<$<CONFIG:Debug>:-g3>
                $<$<CONFIG:Debug>:-DNO_DEBUGGING=1>
                $<$<CONFIG:RelWithDebInfo>:-g3>
                $<$<CONFIG:RelWithDebInfo>:-DNO_DEBUGGING=1>
                $<$<AND:$<NOT:$<CONFIG:Debug>>,$<NOT:$<CONFIG:RelWithDebInfo>>>:-DNO_DEBUGGING=1>
                $<$<AND:$<NOT:$<CONFIG:Debug>>,$<NOT:$<CONFIG:RelWithDebInfo>>>:-DNO_ALL_INTERIOR_POINTERS=1>)
        ELSE()
            SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL}
                $<$<CONFIG:Debug>:-DKEEP_BACK_PTRS=1>
                $<$<CONFIG:Debug>:-DSAVE_CALL_COUNT=8>
                $<$<CONFIG:Debug>:-DDBG_HDRS_ALL=1>
                $<$<CONFIG:Debug>:-DGC_DEBUG>
                $<$<CONFIG:Debug>:-g3>
                $<$<CONFIG:RelWithDebInfo>:-g3>
                # RelWithDebInfo also inherits debug info, but bypasses heavy BDWGC assertions
                $<$<CONFIG:RelWithDebInfo>:-DNO_DEBUGGING=1>
                $<$<AND:$<NOT:$<CONFIG:Debug>>,$<NOT:$<CONFIG:RelWithDebInfo>>>:-DNO_DEBUGGING=1>
                $<$<AND:$<NOT:$<CONFIG:Debug>>,$<NOT:$<CONFIG:RelWithDebInfo>>>:-DNO_ALL_INTERIOR_POINTERS=1>)
        ENDIF()
        set_source_files_properties(${GCUTIL_SRC} PROPERTIES COMPILE_FLAGS "-std=c++11 -fno-rtti")
    ENDIF()
ELSE()
    MESSAGE (FATAL_ERROR "unsupported target")
ENDIF()

IF (GCUTIL_ENABLE_THREADING)
    SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -D_REENTRANT=1 -DGC_THREAD_ISOLATE=1)
    IF (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Test if userfaultfd headers and required structures are available.
        # Some kernels have headers but incomplete definitions.
        # Runtime fallback is handled by GC_dirty_init() in os_dep.c.
        # In cross-compile mode, skip test and let runtime handle it.
        IF (CMAKE_CROSSCOMPILING)
            MESSAGE(STATUS "Cross-compiling: skipping userfaultfd compile test")
        ELSE()
        CHECK_C_SOURCE_COMPILES("
            #include <sys/syscall.h>
            #include <linux/userfaultfd.h>
            #include <unistd.h>
            #include <fcntl.h>
            int main(void) {
                /* Test syscall exists */
                long fd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK);
                /* Test struct uffdio_api and required members */
                struct uffdio_api api;
                api.api = UFFD_API;
                api.features = UFFD_FEATURE_PAGEFAULT_FLAG_WP;
                /* Test struct uffdio_register */
                struct uffdio_register reg;
                reg.mode = UFFDIO_REGISTER_MODE_WP;
                /* Test struct uffdio_writeprotect */
                struct uffdio_writeprotect wp;
                wp.mode = UFFDIO_WRITEPROTECT_MODE_WP;
                /* Test struct uffd_msg */
                struct uffd_msg msg;
                msg.event = 0;
                msg.arg.pagefault.address = 0;
                if (fd >= 0) close((int)fd);
                return 0;
            }
        " UFFDWP_COMPILES)
        IF (NOT UFFDWP_COMPILES)
            MESSAGE(STATUS "userfaultfd headers/structs incomplete, falling back to mprotect VDB")
            SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DNO_UFFDWP_VDB)
        ENDIF()
        ENDIF()
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DNO_GWW_VDB)
    ENDIF()
    IF (GCUTIL_ENABLE_TLS_ACCESS_BY_ADDRESS)
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DENABLE_TLS_ACCESS_BY_ADDRESS=1)
    ENDIF()
    IF (GCUTIL_ENABLE_TLS_ACCESS_BY_PTHREAD_KEY)
        SET (GCUTIL_CFLAGS_INTERNAL ${GCUTIL_CFLAGS_INTERNAL} -DENABLE_TLS_ACCESS_BY_PTHREAD_KEY=1)
    ENDIF()
ENDIF()

# Create the library target (Always STATIC as a sub-component of escargot engine)
ADD_LIBRARY(gc-lib STATIC ${GCUTIL_BDWGC_SRC} ${GCUTIL_SRC})
SET_TARGET_PROPERTIES(gc-lib PROPERTIES POSITION_INDEPENDENT_CODE ON)

# SEPARATE_ARGUMENTS()'s NATIVE_COMMAND mode needs CMake 3.9+ -- pick
# WINDOWS_COMMAND/UNIX_COMMAND explicitly instead (that's all NATIVE_COMMAND
# does internally anyway), which has worked since CMake's earliest 2.x
# releases and keeps this compatible with the 2.8.12 floor declared above.
MACRO (SEPARATE_ARGUMENTS_NATIVE_COMPAT _var)
    IF (WIN32)
        SEPARATE_ARGUMENTS (${_var} WINDOWS_COMMAND)
    ELSE()
        SEPARATE_ARGUMENTS (${_var} UNIX_COMMAND)
    ENDIF()
ENDMACRO()

# Set compile options after target creation
# Using target_compile_options for proper flag handling
IF (GCUTIL_CFLAGS_FROM_EXTERNAL)
    # Convert space-separated string of flags to a CMake list to prevent compilation failures
    # where multiple flags are incorrectly treated as a single quoted argument.
    IF (NOT GCUTIL_CFLAGS_FROM_EXTERNAL MATCHES ";")
        SEPARATE_ARGUMENTS_NATIVE_COMPAT(GCUTIL_CFLAGS_FROM_EXTERNAL)
    ENDIF()
ENDIF()

TARGET_COMPILE_OPTIONS(gc-lib PRIVATE
    ${GCUTIL_CFLAGS_INTERNAL}
    # GCUTIL_CFLAGS: set by build/escargot.cmake (e.g. -DSMALL_CONFIG when
    # ESCARGOT_SMALL_CONFIG is ON) -- was silently dropped during the
    # CMake-conventions refactor, so that option never actually reached
    # bdwgc on ANY platform (not just bare-metal); restored here.
    ${GCUTIL_CFLAGS}
    ${GCUTIL_CFLAGS_FROM_EXTERNAL}
    $<$<CONFIG:MinSizeRel>:${GCUTIL_RELEASE_FLAGS_MINSIZEREL}>
    $<$<AND:$<NOT:$<CONFIG:Debug>>,$<NOT:$<CONFIG:MinSizeRel>>>:${GCUTIL_RELEASE_FLAGS_OTHER}>
)

# Use target_include_directories for include paths
TARGET_INCLUDE_DIRECTORIES(gc-lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
