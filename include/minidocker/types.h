#pragma once
#include <string>
#include <vector>
#include <ctime>
#include <sys/types.h>

struct ContainerConfig {
    std::string id;
    std::string rootfs;
    std::vector<std::string> argv;
    bool tty = false;
    bool detach = false;
    bool interactive = false;
    long memory_mb = 0;
    double cpus = 0.0;
    std::string hostname = "container";
};

struct ContainerState {
    ContainerConfig config;
    pid_t host_pid = -1;
    std::string status = "created"; // created | running | stopped
    std::time_t created_at = 0;
    int exit_code = -1;
};