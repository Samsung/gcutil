CMAKE_MINIMUM_REQUIRED (VERSION 2.8)

#PROJECT (GC)


# CONFIGURATION
SET (CMAKE_VERBOSE_MAKEFILE true)
SET (CMAKE_C_COMPILER gcc)
SET (CMAKE_CXX_COMPILER g++)
 
# FLAGS
SET (GC_CXXFLAGS_DEBUG "${GC_CXXFLAGS_DEBUG} -O0 -g3 -Wall -Wextra -Werror -Wno-unused-variable -DESCARGOT")
SET (GC_CXXFLAGS_RELEASE "${GC_CXXFLAGS_RELEASE} -O2 -g3 -DNDEBUG -Wno-unused-variable -DESCARGOT")

SET (DEBUG_OUTDIR "bdwgc/out/linux/x64/debug.static/.libs")
SET (RELEASE_OUTDIR "bdwgc/out/linux/x64/release.static/.libs")


SET (SRC Allocator.cpp
     LeakChecker.cpp
     bdwgc/allchblk.c
     bdwgc/alloc.c
     bdwgc/backgraph.c
     bdwgc/blacklst.c
     bdwgc/checksums.c
     bdwgc/darwin_stop_world.c
     bdwgc/dbg_mlc.c
     bdwgc/dyn_load.c
     bdwgc/finalize.c
     bdwgc/fnlz_mlc.c
     bdwgc/gcj_mlc.c
     bdwgc/gc_cpp.cc
     bdwgc/gc_cpp.cpp
     bdwgc/gc_dlopen.c
     bdwgc/headers.c
     bdwgc/mach_dep.c
     bdwgc/malloc.c
     bdwgc/mallocx.c
     bdwgc/mark.c
     bdwgc/mark_rts.c
     bdwgc/misc.c
     bdwgc/new_hblk.c
     bdwgc/obj_map.c
     bdwgc/os_dep.c
     bdwgc/pcr_interface.c
     bdwgc/pthread_start.c
     bdwgc/pthread_stop_world.c
     bdwgc/pthread_support.c
     bdwgc/ptr_chck.c
     bdwgc/real_malloc.c
     bdwgc/reclaim.c
     bdwgc/specific.c
     bdwgc/stubborn.c
     bdwgc/thread_local_alloc.c
     bdwgc/typd_mlc.c
     bdwgc/win32_threads.c)


INCLUDE_DIRECTORIES (bdwgc/include)

# LIBRARY BUILD
ADD_LIBRARY (gc_debug STATIC ${SRC})
SET_TARGET_PROPERTIES (gc_debug PROPERTIES
                       COMPILE_FLAGS "${GC_CXXFLAGS_DEBUG}"
                       OUTPUT_NAME "gc"
                       ARCHIVE_OUTPUT_DIRECTORY "${DEBUG_OUTDIR}")

ADD_LIBRARY (gc_release STATIC ${SRC})
SET_TARGET_PROPERTIES (gc_release PROPERTIES
                       COMPILE_FLAGS "${GC_CXXFLAGS_RELEASE}"
                       OUTPUT_NAME "gc"
                       ARCHIVE_OUTPUT_DIRECTORY "${RELEASE_OUTDIR}")
