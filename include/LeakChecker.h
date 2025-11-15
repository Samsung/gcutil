#ifndef __GCUtilLeakChecker__
#define __GCUtilLeakChecker__

#ifdef PROFILE_BDWGC

#include "GCUtil.h"
#include <string>
#include <vector>

namespace GCUtil {

class HeapUsageVisualizer {
public:
    static void initialize();

private:
    static std::string m_outputFile;
};

class GCLeakChecker {
public:
    static void registerAddress(void* ptr, std::string description);
    static void dumpBackTrace(const char* phase = NULL);
    static void setGCPhaseName(std::string name);

private:
    static void unregisterAddress(void* ptr);

    struct LeakCheckedAddr {
        void* ptr;
        std::string description;
        bool deallocated;
    };
    static size_t m_totalFreed;
    static std::vector<LeakCheckedAddr> m_leakCheckedAddrs;
};
}

#endif // PROFILE_BDWGC

#endif // __LeakChecker__
