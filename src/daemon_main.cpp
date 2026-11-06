#include "minidocker/daemon.h"
#include "minidocker/util.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <string>

int main(int argc, char** argv) {
    std::string socket_path = "/tmp/minidocker.sock";
    std::string state_dir;

    const char* xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg) socket_path = std::string(xdg) + "/minidocker.sock";
    const char* home = getenv("HOME");
    state_dir = home ? std::string(home) + "/.local/share/minidocker"
                     : "/tmp/minidocker-state";

    bool rootful = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--socket") && i + 1 < argc) socket_path = argv[++i];
        else if (!strcmp(argv[i], "--state-dir") && i + 1 < argc) state_dir = argv[++i];
        else if (!strcmp(argv[i], "--rootful")) rootful = true;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("Usage: minidockerd [--socket PATH] [--state-dir DIR] [--rootful]\n");
            return 0;
        }
    }

    if (rootful && geteuid() != 0) {
        fprintf(stderr, "--rootful requires root\n");
        return 1;
    }
    if (!md::mkdir_p(state_dir)) {
        fprintf(stderr, "cannot create state dir %s\n", state_dir.c_str());
        return 1;
    }

    md::Daemon d(socket_path, state_dir);
    return d.run();
}