#pragma once
#include "minidocker/types.h"
#include <optional>
#include <string>
#include <vector>

namespace md {

class Store {
public:
    explicit Store(std::string base_dir) : base_(std::move(base_dir)) {}

    bool init();
    std::string container_dir(const std::string& id) const;
    bool save(const ContainerState& st);
    std::optional<ContainerState> load(const std::string& id) const;
    std::vector<ContainerState> list_all() const;

private:
    std::string base_;
};

} // namespace md