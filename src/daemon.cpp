#include "minidocker/daemon.h"
#include "minidocker/container.h"
#include "minidocker/util.h"
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <csignal>

namespace md {

using json = nlohmann::json;

Daemon::Daemon(std::string socket_path, std::string state_dir)
    : socket_path_(std::move(socket_path)),
      state_dir_(std::move(state_dir)),
      store_(state_dir_) {}

Daemon::~Daemon() {
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        unlink(socket_path_.c_str());
    }
}

static bool send_line(int fd, const std::string& s) {
    std::string data = s + "\n";
    ssize_t n = write(fd, data.data(), data.size());
    return n == (ssize_t)data.size();
}

bool Daemon::on_run(int client_fd, const std::string& line) {
    json req = json::parse(line);
    ContainerConfig cfg;
    cfg.id        = gen_id();
    cfg.rootfs    = expand_home(req.value("rootfs", std::string("")));
    if (req.contains("argv") && req["argv"].is_array())
        cfg.argv = req["argv"].get<std::vector<std::string>>();
    cfg.tty         = req.value("tty", false);
    cfg.detach      = req.value("detach", false);
    cfg.interactive = req.value("interactive", false);
    cfg.memory_mb   = req.value("memory_mb", 0L);
    cfg.cpus        = req.value("cpus", 0.0);
    cfg.hostname    = req.value("hostname", std::string("container"));

    if (cfg.rootfs.empty() || cfg.argv.empty()) {
        send_line(client_fd, json{{"ok", false}, {"error", "rootfs and argv required"}}.dump());
        return true;
    }
    if (!file_exists(cfg.rootfs)) {
        send_line(client_fd, json{{"ok", false}, {"error", "rootfs not found: " + cfg.rootfs}}.dump());
        return true;
    }

    auto res = launch_container(cfg, state_dir_);
    if (res.host_pid <= 0) {
        send_line(client_fd, json{{"ok", false}, {"error", "launch failed"}}.dump());
        return true;
    }

    auto rt = std::make_shared<Runtime>();
    rt->st.config     = cfg;
    rt->st.host_pid   = res.host_pid;
    rt->st.status     = "running";
    rt->st.created_at = time(nullptr);

    std::string id_dir  = store_.container_dir(cfg.id);
    std::string log_path = id_dir + "/container.log";
    int log_fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);

    if (cfg.tty) {
        rt->relay = std::make_unique<PtyRelay>(res.pty_master, log_fd);
    } else if (cfg.detach) {
        // Читаем лог в фоне ТОЛЬКО если контейнер detached (-d)
        int rd = res.log_pipe_rd;
        std::string self_id = cfg.id;
        rt->log_thread = std::thread([this, rd, log_fd, self_id] {
            char buf[4096];
            ssize_t n;
            while ((n = read(rd, buf, sizeof(buf))) > 0) {
                if (log_fd >= 0) (void)write(log_fd, buf, (size_t)n);
            }
            close(rd);
            if (log_fd >= 0) close(log_fd);
        });
    }

    store_.save(rt->st);
    {
        std::lock_guard<std::mutex> g(mtx_);
        runtimes_[cfg.id] = rt;
    }

    // JSON response FIRST so client reads it before any PTY bytes.
    send_line(client_fd, json{{"ok", true}, {"id", cfg.id}, {"pid", res.host_pid}}.dump());

    std::string id = cfg.id;
    pid_t pid = res.host_pid;
    auto spawn_monitor = [this, id, pid] {
        std::thread([this, id, pid] {
            int status = 0;
            if (waitpid(pid, &status, 0) > 0) {
                std::lock_guard<std::mutex> g(mtx_);
                auto it = runtimes_.find(id);
                if (it != runtimes_.end()) {
                    it->second->st.status = "stopped";
                    it->second->st.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                    store_.save(it->second->st);
                }
            }
        }).detach();
    };

    if (cfg.tty) {
        if (!cfg.detach) {
            rt->relay->attach(client_fd);
            rt->relay->start();
            spawn_monitor();
            return false; // relay owns client_fd
        }
        rt->relay->start();
        spawn_monitor();
        return true;
    }

    // non-tty
    if (cfg.detach) {
        spawn_monitor();
        return true;
    }

    // non-tty foreground: stream to client and wait
    {
        int rd = res.log_pipe_rd;
        char buf[4096];
        ssize_t n;
        while ((n = read(rd, buf, sizeof(buf))) > 0) {
            if (log_fd >= 0) (void)write(log_fd, buf, (size_t)n);
            if (write(client_fd, buf, (size_t)n) != n) break;
        }
        close(rd);
        if (log_fd >= 0) close(log_fd);
    }
    int status = 0;
    waitpid(res.host_pid, &status, 0);
    {
        std::lock_guard<std::mutex> g(mtx_);
        auto it = runtimes_.find(cfg.id);
        if (it != runtimes_.end()) {
            it->second->st.status = "stopped";
            it->second->st.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            store_.save(it->second->st);
        }
    }
    return true;
}

void Daemon::on_list(int client_fd, const std::string& line) {
    json req = json::parse(line);
    bool all = req.value("all", false);
    json arr = json::array();
    for (auto& st : store_.list_all()) {
        if (!all && st.status != "running") continue;
        arr.push_back({
            {"id",         st.config.id},
            {"pid",        st.host_pid},
            {"status",     st.status},
            {"command",    st.config.argv.empty() ? "" : st.config.argv[0]},
            {"created_at", (long long)st.created_at},
            {"exit_code",  st.exit_code}
        });
    }
    send_line(client_fd, json{{"ok", true}, {"containers", arr}}.dump());
}

void Daemon::on_logs(int client_fd, const std::string& line) {
    json req = json::parse(line);
    std::string id = req.value("id", "");
    bool follow    = req.value("follow", false);
    std::string log_path = store_.container_dir(id) + "/container.log";
    int fd = open(log_path.c_str(), O_RDONLY);
    if (fd < 0) {
        send_line(client_fd, json{{"ok", false}, {"error", "no logs"}}.dump());
        return;
    }
    send_line(client_fd, json{{"ok", true}, {"follow", follow}}.dump());

    char buf[4096];
    ssize_t n;
    off_t pos = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (write(client_fd, buf, (size_t)n) != n) { close(fd); return; }
        pos += n;
    }
    if (!follow) { close(fd); return; }

    while (true) {
        struct stat st {};
        if (stat(log_path.c_str(), &st) == 0 && st.st_size > pos) {
            if (lseek(fd, pos, SEEK_SET) < 0) break;
            while ((n = read(fd, buf, sizeof(buf))) > 0) {
                if (write(client_fd, buf, (size_t)n) != n) { close(fd); return; }
                pos += n;
            }
        }
        usleep(200000);
    }
    close(fd);
}

void Daemon::on_stop(int client_fd, const std::string& line) {
    json req = json::parse(line);
    std::string id = req.value("id", "");
    auto st = store_.load(id);
    if (!st) {
        send_line(client_fd, json{{"ok", false}, {"error", "not found"}}.dump());
        return;
    }
    pid_t pid = st->host_pid;
    if (pid > 0 && st->status == "running") {
        kill(pid, SIGTERM);
        for (int i = 0; i < 50; ++i) {
            if (kill(pid, 0) < 0) break;
            usleep(100000);
        }
        if (kill(pid, 0) == 0) kill(pid, SIGKILL);
        st->status = "stopped";
        store_.save(*st);
        cleanup_cgroups(id);
    }
    send_line(client_fd, json{{"ok", true}}.dump());
}

bool Daemon::on_attach(int client_fd, const std::string& line) {
    json req = json::parse(line);
    std::string id = req.value("id", "");
    auto st = store_.load(id);
    if (!st || st->status != "running") {
        send_line(client_fd, json{{"ok", false}, {"error", "not running"}}.dump());
        return true;
    }
    if (!st->config.tty) {
        send_line(client_fd, json{{"ok", false}, {"error", "container has no tty"}}.dump());
        return true;
    }
    std::shared_ptr<Runtime> rt;
    {
        std::lock_guard<std::mutex> g(mtx_);
        auto it = runtimes_.find(id);
        if (it != runtimes_.end()) rt = it->second;
    }
    if (!rt || !rt->relay) {
        send_line(client_fd, json{{"ok", false}, {"error", "no relay"}}.dump());
        return true;
    }
    send_line(client_fd, json{{"ok", true}}.dump());
    rt->relay->attach(client_fd);
    return false; // relay owns client_fd
}

bool Daemon::handle_client(int client_fd) {
    std::string line;
    char c;
    while (read(client_fd, &c, 1) == 1) {
        if (c == '\n') break;
        line += c;
        if (line.size() > 1024 * 1024) return true;
    }
    if (line.empty()) return true;

    try {
        json j = json::parse(line);
        std::string op = j.value("op", "");
        if (op == "run")    return on_run(client_fd, line);
        if (op == "list")   { on_list(client_fd, line);   return true; }
        if (op == "logs")   { on_logs(client_fd, line);   return true; }
        if (op == "stop")   { on_stop(client_fd, line);   return true; }
        if (op == "attach") return on_attach(client_fd, line);
        send_line(client_fd, json{{"ok", false}, {"error", "unknown op"}}.dump());
    } catch (std::exception& e) {
        send_line(client_fd, json{{"ok", false}, {"error", e.what()}}.dump());
    }
    return true;
}

int Daemon::run() {
    if (!store_.init()) {
        fprintf(stderr, "cannot create state dir %s\n", state_dir_.c_str());
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    unlink(socket_path_.c_str());
    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
    if (bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(listen_fd_, 16) < 0) { perror("listen"); return 1; }
    (void)chmod(socket_path_.c_str(), 0666);

    fprintf(stderr, "minidockerd listening on %s\n", socket_path_.c_str());
    fprintf(stderr, "state dir: %s\n", state_dir_.c_str());

    while (true) {
        int cfd = accept(listen_fd_, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        std::thread([this, cfd] {
            bool close_fd = handle_client(cfd);
            if (close_fd) close(cfd);
        }).detach();
    }
    return 0;
}

} // namespace md