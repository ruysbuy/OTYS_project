#pragma once
#include <string>
#include <vector>
#include <sys/types.h>

namespace md {

std::string read_file(const std::string& path);
bool write_file(const std::string& path, const std::string& data);
bool mkdir_p(const std::string& path, mode_t mode = 0755);
std::vector<std::string> split(const std::string& s, char sep);
std::string gen_id();
std::string expand_home(const std::string& path);
bool file_exists(const std::string& path);
std::string errno_str();

} // namespace md