#pragma once
#include "minidocker/types.h"
#include <sys/types.h>
#include <string>

namespace md {

struct LaunchResult {
    pid_t host_pid = -1;
    int pty_master = -1;
    int log_pipe_rd = -1;
};

LaunchResult launch_container(const ContainerConfig& cfg, const std::string& state_dir);

bool setup_cgroups(const std::string& id, pid_t host_pid, long memory_mb, double cpus);
bool cleanup_cgroups(const std::string& id);

} // namespace md