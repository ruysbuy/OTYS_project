#include "minidocker/store.h"
#include "minidocker/util.h"
#include <nlohmann/json.hpp>
#include <dirent.h>
#include <fstream>

namespace md {

using json = nlohmann::json;

bool Store::init() {
    return mkdir_p(base_ + "/containers");
}

std::string Store::container_dir(const std::string& id) const {
    return base_ + "/containers/" + id;
}

bool Store::save(const ContainerState& st) {
    std::string dir = container_dir(st.config.id);
    if (!mkdir_p(dir)) return false;
    json j;
    j["id"]          = st.config.id;
    j["rootfs"]      = st.config.rootfs;
    j["argv"]        = st.config.argv;
    j["tty"]         = st.config.tty;
    j["detach"]      = st.config.detach;
    j["interactive"] = st.config.interactive;
    j["memory_mb"]   = st.config.memory_mb;
    j["cpus"]        = st.config.cpus;
    j["hostname"]    = st.config.hostname;
    j["host_pid"]    = st.host_pid;
    j["status"]      = st.status;
    j["created_at"]  = (long long)st.created_at;
    j["exit_code"]   = st.exit_code;
    std::ofstream f(dir + "/state.json");
    if (!f) return false;
    f << j.dump(2);
    return true;
}

std::optional<ContainerState> Store::load(const std::string& id) const {
    std::string path = container_dir(id) + "/state.json";
    std::ifstream f(path);
    if (!f) return std::nullopt;
    json j;
    try { f >> j; } catch (...) { return std::nullopt; }
    ContainerState st;
    st.config.id          = j.value("id", "");
    st.config.rootfs      = j.value("rootfs", "");
    if (j.contains("argv") && j["argv"].is_array())
        st.config.argv = j["argv"].get<std::vector<std::string>>();
    st.config.tty         = j.value("tty", false);
    st.config.detach      = j.value("detach", false);
    st.config.interactive = j.value("interactive", false);
    st.config.memory_mb   = j.value("memory_mb", 0L);
    st.config.cpus        = j.value("cpus", 0.0);
    st.config.hostname    = j.value("hostname", std::string("container"));
    st.host_pid           = j.value("host_pid", -1);
    st.status             = j.value("status", std::string("created"));
    st.created_at         = (std::time_t)j.value("created_at", 0LL);
    st.exit_code          = j.value("exit_code", -1);
    return st;
}

std::vector<ContainerState> Store::list_all() const {
    std::vector<ContainerState> out;
    std::string cdir = base_ + "/containers";
    DIR* d = opendir(cdir.c_str());
    if (!d) return out;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        auto st = load(e->d_name);
        if (st) out.push_back(*st);
    }
    closedir(d);
    return out;
}

} // namespace md