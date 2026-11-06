#include "minidocker/pty_relay.h"
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <vector>

namespace md {

PtyRelay::PtyRelay(int pty_master, int log_fd)
    : pty_master_(pty_master), log_fd_(log_fd) {}

PtyRelay::~PtyRelay() { stop(); }

void PtyRelay::start() {
    running_ = true;
    thread_ = std::thread([this] { loop(); });
}

void PtyRelay::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (pty_master_ >= 0) { close(pty_master_); pty_master_ = -1; }
    if (log_fd_     >= 0) { close(log_fd_);     log_fd_ = -1; }
}

void PtyRelay::attach(int client_fd) {
    std::lock_guard<std::mutex> g(mtx_);
    clients_.insert(client_fd);
}

void PtyRelay::detach(int client_fd) {
    std::lock_guard<std::mutex> g(mtx_);
    clients_.erase(client_fd);
}

void PtyRelay::loop() {
    std::vector<char> buf(4096);
    while (running_) {
        std::vector<struct pollfd> fds;
        {
            std::lock_guard<std::mutex> g(mtx_);
            fds.push_back({pty_master_, POLLIN, 0});
            for (int c : clients_) fds.push_back({c, POLLIN, 0});
        }

        int n = poll(fds.data(), fds.size(), 200);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) continue;

        // PTY -> log + clients
        if (fds[0].revents & POLLIN) {
            ssize_t r = read(pty_master_, buf.data(), buf.size());
            if (r > 0) {
                if (log_fd_ >= 0) (void)write(log_fd_, buf.data(), (size_t)r);
                std::lock_guard<std::mutex> g(mtx_);
                for (auto it = clients_.begin(); it != clients_.end(); ) {
                    if (write(*it, buf.data(), (size_t)r) != r) {
                        close(*it);
                        it = clients_.erase(it);
                    } else {
                        ++it;
                    }
                }
            } else {
                // EOF or fatal — container exited. Close all clients.
                std::lock_guard<std::mutex> g(mtx_);
                for (int c : clients_) close(c);
                clients_.clear();
                break;
            }
        }

        // clients -> PTY
        for (size_t i = 1; i < fds.size(); ++i) {
            if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                ssize_t r = read(fds[i].fd, buf.data(), buf.size());
                if (r > 0) {
                    (void)write(pty_master_, buf.data(), (size_t)r);
                } else {
                    std::lock_guard<std::mutex> g(mtx_);
                    clients_.erase(fds[i].fd);
                    close(fds[i].fd);
                }
            }
        }
    }
}

} // namespace md