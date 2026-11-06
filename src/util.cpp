#include "minidocker/util.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <sstream>
#include <random>
#include <pwd.h>
#include <cstdlib>

namespace md {

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool write_file(const std::string& path, const std::string& data) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    ssize_t n = ::write(fd, data.data(), data.size());
    ::close(fd);
    return n == (ssize_t)data.size();
}

bool mkdir_p(const std::string& path, mode_t mode) {
    if (path.empty() || path == "/") return true;
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if (path[i] == '/' || i + 1 == path.size()) {
            if (cur == "/") continue;
            std::string d = cur;
            if (d.back() == '/') d.pop_back();
            if (mkdir(d.c_str(), mode) < 0 && errno != EEXIST) return false;
        }
    }
    return true;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty() || !s.empty()) out.push_back(cur);
    return out;
}

std::string gen_id() {
    static std::mt19937_64 rng(std::random_device{}());
    static const char* hex = "0123456789abcdef";
    std::string s(12, '0');
    for (auto& c : s) c = hex[rng() % 16];
    return s;
}

std::string expand_home(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    const char* home = getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (!home) return path;
    return std::string(home) + path.substr(1);
}

bool file_exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

std::string errno_str() { return std::string(strerror(errno)); }

} // namespace md