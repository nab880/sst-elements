// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2026, NTESS
// All rights reserved.

#ifndef _QUETZ_PLUGIN_REGISTRY_H
#define _QUETZ_PLUGIN_REGISTRY_H

#include <functional>
#include <string>
#include <vector>

namespace SST {
namespace Quetz {

template <typename T>
class Registry {
public:
    static Registry& instance() {
        static Registry inst;
        return inst;
    }

    void add(const std::string& prefix, std::function<T*()> factory) {
        entries_.emplace_back(prefix, std::move(factory));
    }

    T* findByPrefix(const std::string& target_name) const {
        const std::function<T*()>* best_factory = nullptr;
        size_t                       best_len   = 0;
        for (const auto& e : entries_) {
            if (e.first.empty()) continue;
            if (target_name.size() >= e.first.size() &&
                target_name.compare(0, e.first.size(), e.first) == 0 &&
                e.first.size() > best_len)
            {
                best_factory = &e.second;
                best_len     = e.first.size();
            }
        }
        if (best_factory)
            return (*best_factory)();
        for (const auto& e : entries_) {
            if (e.first.empty())
                return e.second();
        }
        return nullptr;
    }

private:
    std::vector<std::pair<std::string, std::function<T*()>>> entries_;
};

#define QUETZ_REGISTER_CLASSIFIER(prefix, cls)                          \
    static bool _quetz_reg_classifier_##cls = []() {                      \
        Registry<class InsnClassifier>::instance().add(                   \
            prefix, []() {                                                \
                static cls instance;                                      \
                return &instance;                                         \
            });                                                           \
        return true;                                                      \
    }()

#define QUETZ_REGISTER_MEM_HANDLER(prefix, cls)                           \
    static bool _quetz_reg_mem_handler_##cls = []() {                     \
        Registry<class MemAccessHandler>::instance().add(                 \
            prefix, []() {                                                \
                static cls instance;                                      \
                return &instance;                                         \
            });                                                           \
        return true;                                                      \
    }()

} // namespace Quetz
} // namespace SST

#endif // _QUETZ_PLUGIN_REGISTRY_H
