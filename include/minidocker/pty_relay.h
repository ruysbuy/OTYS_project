#pragma once
#include <set>
#include <mutex>
#include <thread>
#include <atomic>

namespace md {

class PtyRelay {
public:
    PtyRelay(int pty_master, int log_fd);
    ~PtyRelay();

    void start();
    void stop();
    void attach(int client_fd);
    void detach(int client_fd);

private:
    void loop();

    int pty_master_;
    int log_fd_;
    std::mutex mtx_;
    std::set<int> clients_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace md