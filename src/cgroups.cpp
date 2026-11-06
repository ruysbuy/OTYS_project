#include "minidocker/container.h"
#include "minidocker/util.h"
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <string>

namespace md {

static const char* kCgroupRoot = "/sys/fs/cgroup";

bool setup_cgroups(const std::string& id, pid_t host_pid, long memory_mb, double cpus) {
    if (!file_exists(std::string(kCgroupRoot) + "/cgroup.controllers")) {
        return false; // cgroups v2 not mounted
    }
    std::string base = std::string(kCgroupRoot) + "/minidocker";
    mkdir_p(base);
    (void)write_file(base + "/cgroup.subtree_control", "+memory +cpu +pids");

    std::string dir = base + "/" + id;
    if (!mkdir_p(dir)) return false;

    if (memory_mb > 0) {
        (void)write_file(dir + "/memory.max",
                         std::to_string(memory_mb * 1024L * 1024L));
        (void)write_file(dir + "/memory.swap.max", "0");
    }
    if (cpus > 0) {
        long quota = (long)(cpus * 100000.0);
        (void)write_file(dir + "/cpu.max",
                         std::to_string(quota) + " 100000");
    }
    (void)write_file(dir + "/pids.max", "256");
    (void)write_file(dir + "/cgroup.procs", std::to_string(host_pid));
    return true;
}

bool cleanup_cgroups(const std::string& id) {
    std::string dir = std::string(kCgroupRoot) + "/minidocker/" + id;
    if (file_exists(dir)) {
        (void)write_file(dir + "/cgroup.kill", "1");
        rmdir(dir.c_str());
    }
    return true;
}

} // namespace md