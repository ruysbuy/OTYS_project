#include "minidocker/util.h"
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <termios.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <string>
#include <vector>
#include <algorithm>
#include <csignal>

using json = nlohmann::json;

static std::string g_socket;

static int connect_daemon() {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_socket.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "cannot connect to daemon at %s: %s\n",
                g_socket.c_str(), strerror(errno));
        exit(1);
    }
    return fd;
}

static std::string recv_line(int fd) {
    std::string out;
    char c;
    while (read(fd, &c, 1) == 1) {
        if (c == '\n') break;
        out += c;
    }
    return out;
}

static void send_json(int fd, const json& j) {
    std::string s = j.dump() + "\n";
    ssize_t n = write(fd, s.data(), s.size());
    (void)n;
}

static struct termios g_saved_tio;
static bool g_raw = false;

static void enter_raw() {
    if (!isatty(STDIN_FILENO)) return;
    struct termios t {};
    if (tcgetattr(STDIN_FILENO, &g_saved_tio) < 0) return;
    t = g_saved_tio;
    cfmakeraw(&t);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
    g_raw = true;
}

static void leave_raw() {
    if (g_raw) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_tio);
        g_raw = false;
    }
}

static void relay_loop(int fd) {
    char buf[4096];
    while (true) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        FD_SET(STDIN_FILENO, &rfds);
        int maxfd = std::max(fd, (int)STDIN_FILENO) + 1;
        int n = select(maxfd, &rfds, nullptr, nullptr, nullptr);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (FD_ISSET(fd, &rfds)) {
            ssize_t r = read(fd, buf, sizeof(buf));
            if (r <= 0) break;
            ssize_t w = write(STDOUT_FILENO, buf, (size_t)r);
            (void)w;
        }
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
            if (r <= 0) break;
            ssize_t w = write(fd, buf, (size_t)r);
            (void)w;
        }
    }
}

static int cmd_run(const std::vector<std::string>& args) {
    std::string rootfs;
    std::string hostname = "container";
    bool detach = false, tty = false, interactive = false;
    long mem = 0;
    double cpus = 0.0;
    std::vector<std::string> cmd;

    size_t i = 0;
    for (; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--rootfs" && i + 1 < args.size()) rootfs = args[++i];
        else if (a == "--memory" && i + 1 < args.size()) mem = std::stol(args[++i]);
        else if (a == "--cpus" && i + 1 < args.size()) cpus = std::stod(args[++i]);
        else if (a == "--hostname" && i + 1 < args.size()) hostname = args[++i];
        else if (a == "--") { ++i; break; }
        else if (a.size() > 1 && a[0] == '-' && a[1] != '-') {
            // Обработка слитных коротких флагов: -td, -it и т.д.
            for (size_t k = 1; k < a.size(); ++k) {
                if (a[k] == 'd') detach = true;
                else if (a[k] == 't') tty = true;
                else if (a[k] == 'i') interactive = true;
                else { fprintf(stderr, "unknown flag -%c\n", a[k]); return 1; }
            }
        }
        else if (a == "-d" || a == "--detach") detach = true;
        else if (a == "-t" || a == "--tty") tty = true;
        else if (a == "-i" || a == "--interactive") interactive = true;
        else if (!a.empty() && a[0] == '-') {
            fprintf(stderr, "unknown flag %s\n", a.c_str());
            return 1;
        } else {
            cmd.push_back(a);
        }
    }
    for (; i < args.size(); ++i) cmd.push_back(args[i]);

    if (rootfs.empty() || cmd.empty()) {
        fprintf(stderr,
            "usage: minidocker run --rootfs DIR [-d] [-t] [-i] "
            "[--memory MB] [--cpus N] [--hostname NAME] -- CMD [ARGS...]\n");
        return 1;
    }

    int fd = connect_daemon();
    send_json(fd, json{
        {"op", "run"}, {"rootfs", rootfs}, {"argv", cmd},
        {"tty", tty}, {"detach", detach}, {"interactive", interactive},
        {"memory_mb", mem}, {"cpus", cpus}, {"hostname", hostname}
    });

    std::string line = recv_line(fd);
    if (line.empty()) { fprintf(stderr, "no response\n"); return 1; }
    json resp = json::parse(line);
    if (!resp.value("ok", false)) {
        fprintf(stderr, "error: %s\n", resp.value("error", "unknown").c_str());
        return 1;
    }
    std::string id = resp.value("id", "");

    if (detach) {
        printf("%s\n", id.c_str());
        close(fd);
        return 0;
    }

    if (tty) {
        enter_raw();
        relay_loop(fd);
        leave_raw();
    } else {
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            ssize_t w = write(STDOUT_FILENO, buf, (size_t)n);
            (void)w;
        }
    }
    close(fd);
    return 0;
}

static int cmd_list(bool all) {
    int fd = connect_daemon();
    send_json(fd, json{{"op", "list"}, {"all", all}});
    std::string line = recv_line(fd);
    close(fd);
    if (line.empty()) return 1;
    json resp = json::parse(line);
    if (!resp.value("ok", false)) {
        fprintf(stderr, "error: %s\n", resp.value("error", "unknown").c_str());
        return 1;
    }
    printf("%-14s %-8s %-10s %s\n", "ID", "PID", "STATUS", "COMMAND");
    for (auto& c : resp["containers"]) {
        printf("%-14s %-8d %-10s %s\n",
            c["id"].get<std::string>().c_str(),
            c["pid"].get<int>(),
            c["status"].get<std::string>().c_str(),
            c["command"].get<std::string>().c_str());
    }
    return 0;
}

static int cmd_logs(const std::vector<std::string>& args) {
    bool follow = false;
    std::string id;
    for (auto& a : args) {
        if (a == "-f" || a == "--follow") follow = true;
        else id = a;
    }
    if (id.empty()) { fprintf(stderr, "usage: minidocker logs [-f] ID\n"); return 1; }
    int fd = connect_daemon();
    send_json(fd, json{{"op", "logs"}, {"id", id}, {"follow", follow}});
    std::string line = recv_line(fd);
    json resp = json::parse(line);
    if (!resp.value("ok", false)) {
        fprintf(stderr, "error: %s\n", resp.value("error", "unknown").c_str());
        close(fd); return 1;
    }
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        ssize_t w = write(STDOUT_FILENO, buf, (size_t)n);
        (void)w;
    }
    close(fd);
    return 0;
}

static int cmd_stop(const std::vector<std::string>& args) {
    if (args.empty()) { fprintf(stderr, "usage: minidocker stop ID\n"); return 1; }
    int fd = connect_daemon();
    send_json(fd, json{{"op", "stop"}, {"id", args[0]}});
    std::string line = recv_line(fd);
    close(fd);
    json resp = json::parse(line);
    if (!resp.value("ok", false)) {
        fprintf(stderr, "error: %s\n", resp.value("error", "unknown").c_str());
        return 1;
    }
    return 0;
}

static int cmd_attach(const std::vector<std::string>& args) {
    if (args.empty()) { fprintf(stderr, "usage: minidocker attach ID\n"); return 1; }
    int fd = connect_daemon();
    send_json(fd, json{{"op", "attach"}, {"id", args[0]}});
    std::string line = recv_line(fd);
    json resp = json::parse(line);
    if (!resp.value("ok", false)) {
        fprintf(stderr, "error: %s\n", resp.value("error", "unknown").c_str());
        close(fd); return 1;
    }
    enter_raw();
    relay_loop(fd);
    leave_raw();
    close(fd);
    return 0;
}

int main(int argc, char** argv) {
    const char* xdg = getenv("XDG_RUNTIME_DIR");
    g_socket = (xdg && *xdg) ? std::string(xdg) + "/minidocker.sock"
                             : "/tmp/minidocker.sock";
    if (const char* env = getenv("MINIDOCKER_SOCKET")) g_socket = env;

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--socket" && i + 1 < argc) g_socket = argv[++i];
        else args.push_back(a);
    }
    if (args.empty()) {
        fprintf(stderr, "usage: minidocker <run|ps|logs|stop|attach> ...\n");
        return 1;
    }

    std::string cmd = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());

    signal(SIGPIPE, SIG_IGN);

    if (cmd == "run")    return cmd_run(rest);
    if (cmd == "ps" || cmd == "list") {
        bool all = false;
        for (auto& a : rest) if (a == "-a" || a == "--all") all = true;
        return cmd_list(all);
    }
    if (cmd == "logs")   return cmd_logs(rest);
    if (cmd == "stop")   return cmd_stop(rest);
    if (cmd == "attach") return cmd_attach(rest);

    fprintf(stderr, "unknown command: %s\n", cmd.c_str());
    return 1;
}