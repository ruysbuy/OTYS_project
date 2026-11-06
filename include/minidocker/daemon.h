#pragma once
#include "minidocker/types.h"
#include "minidocker/store.h"
#include "minidocker/pty_relay.h"
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <atomic>
#include <string>

namespace md {

class Daemon {
public:
    Daemon(std::string socket_path, std::string state_dir);
    ~Daemon();

    int run();

private:
    struct Runtime {
        ContainerState st;
        std::unique_ptr<PtyRelay> relay;
        std::thread log_thread;
    };

    // returns true if caller should close client_fd, false if handoff (relay owns it)
    bool handle_client(int client_fd);

    bool on_run(int client_fd, const std::string& line);
    void on_list(int client_fd, const std::string& line);
    void on_logs(int client_fd, const std::string& line);
    void on_stop(int client_fd, const std::string& line);
    bool on_attach(int client_fd, const std::string& line);

    std::string socket_path_;
    std::string state_dir_;
    Store store_;
    int listen_fd_ = -1;
    std::mutex mtx_;
    std::unordered_map<std::string, std::shared_ptr<Runtime>> runtimes_;
};

} // namespace md