#include "perf.hpp"

#include <sys/resource.h>
#include <sys/stat.h>

#include <cstdlib>
#include <string>

namespace {

bool path_exists(const char* path) {
    struct stat st {};
    return stat(path, &st) == 0;
}

}  // namespace

double peak_rss_mb() {
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0.0;
    }
#if defined(__APPLE__)
    return static_cast<double>(usage.ru_maxrss) / (1024.0 * 1024.0);
#else
    return static_cast<double>(usage.ru_maxrss) / 1024.0;
#endif
}

std::string detect_hardware_acceleration() {
    std::string backends;
    auto add = [&](const char* name) {
        if (!backends.empty()) {
            backends += " + ";
        }
        backends += name;
    };
    if (std::getenv("CUDA_VISIBLE_DEVICES") != nullptr || path_exists("/dev/nvidia0")) {
        add("CUDA");
    }
    if (path_exists("/dev/kfd") || std::getenv("ROCR_VISIBLE_DEVICES") != nullptr) {
        add("ROCm/RADV");
    }
#if defined(__APPLE__)
    add("Apple Silicon / MPS");
#endif
    if (backends.empty()) {
        add("CPU / Vulkan / Generic");
    }
    return backends;
}
