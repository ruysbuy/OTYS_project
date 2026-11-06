#include "minidocker/container.h"
#include "minidocker/util.h"
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <net/if.h>
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <pty.h>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace md {

static void sync_send(int fd, char c) { ssize_t n = write(fd, &c, 1); (void)n; }
static bool sync_recv(int fd, char expect) {
    char c; ssize_t n = read(fd, &c, 1);
    return n == 1 && c == expect;
}

// Parent writes uid_map/gid_map for a child that just entered a new user ns.
static bool write_user_map(pid_t child_pid) {
    std::string base = "/proc/" + std::to_string(child_pid);
    (void)write_file(base + "/setgroups", "deny");
    std::string uid_map = "0 " + std::to_string(getuid()) + " 1\n";
    std::string gid_map = "0 " + std::to_string(getgid()) + " 1\n";
    if (!write_file(base + "/uid_map", uid_map)) {
        fprintf(stderr, "[userns] uid_map write failed: %s\n", strerror(errno));
        return false;
    }
    if (!write_file(base + "/gid_map", gid_map)) {
        fprintf(stderr, "[userns] gid_map write failed: %s\n", strerror(errno));
        return false;
    }
    return true;
}

static bool setup_rootfs(const ContainerConfig& cfg, const std::string& state_dir) {
    std::string merged = state_dir + "/merged";
    std::string upper  = state_dir + "/upper";
    std::string work   = state_dir + "/work";

    if (!mkdir_p(merged) || !mkdir_p(upper) || !mkdir_p(work)) return false;
    if (!file_exists(cfg.rootfs)) {
        fprintf(stderr, "[rootfs] not found: %s\n", cfg.rootfs.c_str());
        return false;
    }

    std::string opts = "lowerdir=" + cfg.rootfs + ",upperdir=" + upper +
                       ",workdir=" + work;
    if (mount("overlay", merged.c_str(), "overlay", 0, opts.c_str()) < 0) {
        // Fallback to bind mount (rootless w/o unprivileged overlayfs)
        if (mount(cfg.rootfs.c_str(), merged.c_str(), nullptr, MS_BIND | MS_REC, nullptr) < 0) {
            fprintf(stderr, "[rootfs] bind mount failed: %s\n", strerror(errno));
            return false;
        }
    }

    std::string put_old = merged + "/.oldroot";
    if (mkdir(put_old.c_str(), 0755) < 0 && errno != EEXIST) return false;
    if (syscall(SYS_pivot_root, merged.c_str(), put_old.c_str()) < 0) {
        if (chroot(merged.c_str()) < 0) {
            fprintf(stderr, "[rootfs] chroot failed: %s\n", strerror(errno));
            return false;
        }
        if (chdir("/") < 0) return false;
    } else {
        if (chdir("/") < 0) return false;
        umount2("/.oldroot", MNT_DETACH);
        rmdir("/.oldroot");
    }

    if (!file_exists("/proc")) mkdir("/proc", 0755);
    // Обязательно передаем MS_NOSUID | MS_NODEV | MS_NOEXEC для работы в User NS
    if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr) < 0) {
        // Fallback на случай экзотических ограничений
        if (mount("/proc", "/proc", nullptr, MS_BIND | MS_REC, nullptr) < 0) {
            fprintf(stderr, "[rootfs] /proc mount failed: %s\n", strerror(errno));
            return false;
        }
    }

    if (!file_exists("/dev")) mkdir("/dev", 0755);
    (void)mount("tmpfs", "/dev", "tmpfs", MS_NOSUID | MS_STRICTATIME, "mode=755");

    struct { const char* n; mode_t m; dev_t d; } devs[] = {
        {"/dev/null",    S_IFCHR | 0666, makedev(1, 3)},
        {"/dev/zero",    S_IFCHR | 0666, makedev(1, 5)},
        {"/dev/full",    S_IFCHR | 0666, makedev(1, 7)},
        {"/dev/random",  S_IFCHR | 0666, makedev(1, 8)},
        {"/dev/urandom", S_IFCHR | 0666, makedev(1, 9)},
        {"/dev/tty",     S_IFCHR | 0666, makedev(5, 0)},
    };
    for (auto& dv : devs) (void)mknod(dv.n, dv.m, dv.d);
    if (!file_exists("/dev/fd"))     (void)symlink("/proc/self/fd", "/dev/fd");
    if (!file_exists("/dev/stdin"))  (void)symlink("/proc/self/fd/0", "/dev/stdin");
    if (!file_exists("/dev/stdout")) (void)symlink("/proc/self/fd/1", "/dev/stdout");
    if (!file_exists("/dev/stderr")) (void)symlink("/proc/self/fd/2", "/dev/stderr");
    return true;
}

static void bring_up_loopback() {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    struct ifreq ifr {};
    std::strncpy(ifr.ifr_name, "lo", IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        (void)ioctl(s, SIOCSIFFLAGS, &ifr);
    }
    close(s);
}

LaunchResult launch_container(const ContainerConfig& cfg, const std::string& state_dir) {
    LaunchResult res;
    std::string id_dir = state_dir + "/containers/" + cfg.id;
    mkdir_p(id_dir);

    int pty_master = -1, pty_slave = -1;
    int log_pipe[2] = {-1, -1};

    if (cfg.tty) {
        if (openpty(&pty_master, &pty_slave, nullptr, nullptr, nullptr) < 0) {
            fprintf(stderr, "openpty failed: %s\n", strerror(errno));
            return res;
        }
    } else {
        if (pipe(log_pipe) < 0) return res;
    }

    int ready[2], go[2];
    if (pipe(ready) < 0 || pipe(go) < 0) {
        if (pty_master >= 0) close(pty_master);
        if (pty_slave  >= 0) close(pty_slave);
        if (log_pipe[0] >= 0) close(log_pipe[0]);
        if (log_pipe[1] >= 0) close(log_pipe[1]);
        return res;
    }

    // Создаем User Namespace только если мы НЕ root
    bool use_userns = (geteuid() != 0);
    int flags = CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNET | SIGCHLD;
    if (use_userns) {
        flags |= CLONE_NEWUSER;
    }

    pid_t pid = syscall(SYS_clone, flags, nullptr, nullptr, nullptr, 0);
    if (pid < 0) {
        fprintf(stderr, "clone failed: %s\n", strerror(errno));
        if (pty_master >= 0) close(pty_master);
        if (pty_slave  >= 0) close(pty_slave);
        if (log_pipe[0] >= 0) close(log_pipe[0]);
        if (log_pipe[1] >= 0) close(log_pipe[1]);
        close(ready[0]); close(ready[1]); close(go[0]); close(go[1]);
        return res;
    }

    if (pid == 0) {
        // ---- CHILD ----
        close(ready[0]); close(go[1]);
        if (pty_master >= 0) close(pty_master);
        if (log_pipe[0] >= 0) close(log_pipe[0]);

        if (use_userns) {
            // Signal 'R': entered userns, waiting for uid/gid map.
            sync_send(ready[1], 'R');
            if (!sync_recv(go[0], 'G')) _exit(1);
        } else {
            // Если без userns, просто ждем 'G' от родителя
            if (!sync_recv(go[0], 'G')) _exit(1);
        }

        (void)sethostname(cfg.hostname.c_str(), cfg.hostname.size());
        if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) < 0) {
            fprintf(stderr, "[child] MS_PRIVATE failed: %s\n", strerror(errno));
            sync_send(ready[1], 'E'); _exit(1);
        }
        if (!setup_rootfs(cfg, id_dir)) {
            sync_send(ready[1], 'E'); _exit(1);
        }
        bring_up_loopback();

        if (pty_slave >= 0) {
            setsid();
            ioctl(pty_slave, TIOCSCTTY, 0);
            dup2(pty_slave, 0); dup2(pty_slave, 1); dup2(pty_slave, 2);
            if (pty_slave > 2) close(pty_slave);
        } else {
            dup2(log_pipe[1], 1); dup2(log_pipe[1], 2);
            if (log_pipe[1] > 2) close(log_pipe[1]);
        }

        sync_send(ready[1], 'S');
        close(ready[1]); close(go[0]);

        std::vector<char*> argv;
        for (auto& a : cfg.argv) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        fprintf(stderr, "execvp %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    // ---- PARENT ----
    close(ready[1]); close(go[0]);
    if (pty_slave >= 0) close(pty_slave);
    if (log_pipe[1] >= 0) close(log_pipe[1]);

    if (use_userns) {
        if (!sync_recv(ready[0], 'R')) {
            close(ready[0]); close(go[1]);
            if (pty_master >= 0) close(pty_master);
            if (log_pipe[0] >= 0) close(log_pipe[0]);
            waitpid(pid, nullptr, 0);
            return res;
        }

        if (!write_user_map(pid)) {
            close(ready[0]); close(go[1]);
            if (pty_master >= 0) close(pty_master);
            if (log_pipe[0] >= 0) close(log_pipe[0]);
            kill(pid, SIGKILL); waitpid(pid, nullptr, 0);
            return res;
        }
    }

    sync_send(go[1], 'G');
    close(go[1]);

    if (!sync_recv(ready[0], 'S')) {
        close(ready[0]);
        if (pty_master >= 0) close(pty_master);
        if (log_pipe[0] >= 0) close(log_pipe[0]);
        waitpid(pid, nullptr, 0);
        return res;
    }
    close(ready[0]);

    setup_cgroups(cfg.id, pid, cfg.memory_mb, cfg.cpus);

    res.host_pid = pid;
    res.pty_master = pty_master;
    res.log_pipe_rd = log_pipe[0];
    return res;
}

} // namespace md