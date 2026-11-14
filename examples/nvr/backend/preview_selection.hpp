#pragma once
#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <vector>
namespace amdva::nvr::detail {
// Desired source IDs outlive cached status/slot assignments. Call contains()
// while holding the slot mutex both when binding and when applying a layout.
class PreviewSelection {
    std::shared_ptr<const std::set<std::string>> ids_;
public:
    void set(const std::vector<std::string> &ids) {
        std::shared_ptr<const std::set<std::string>> next=
            std::make_shared<const std::set<std::string>>(ids.begin(),ids.end());
        std::atomic_store(&ids_,std::move(next));
    }
    bool contains(const std::string &id) const {
        const auto ids=std::atomic_load(&ids_);
        return !id.empty() && (!ids || ids->count(id));
    }
};
}
