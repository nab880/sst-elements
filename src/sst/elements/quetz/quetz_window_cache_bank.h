#ifndef SST_QUETZ_WINDOW_CACHE_BANK_H
#define SST_QUETZ_WINDOW_CACHE_BANK_H

#include "quetz_window_cache.h"
#include <unordered_map>

namespace SST { namespace Quetz {

class WindowCacheBank {
public:
    struct Pending { uint32_t vcpu; bool read; };

    void configure(uint32_t cores, uint64_t base, uint64_t size) {
        if (cores < 1 || cores > 2 || active())
            throw std::invalid_argument("window cache bank requires one or two idle CPUs");
        std::vector<State> next(cores);
        for (auto& state : next) state.cache.configure(base, size);
        states_ = std::move(next);
    }

    WindowDataCache& cache(uint32_t vcpu) { return states_.at(vcpu).cache; }
    const WindowDataCache& cache(uint32_t vcpu) const { return states_.at(vcpu).cache; }

    void addRegion(uint64_t base, uint64_t size) {
        if (active()) throw std::logic_error("adding a cache region during an active transaction");
        for (auto& state : states_) state.cache.addRegion(base, size);
    }

    bool active() const {
        if (!pending_.empty()) return true;
        for (const auto& state : states_)
            if (state.cache.active()) return true;
        return false;
    }

    void issued(uint32_t vcpu, uint64_t id, bool read) {
        auto& state = states_.at(vcpu);
        if (!state.cache.active() || state.outstanding || pending_.count(id))
            throw std::logic_error("overlapping window cache backing request");
        pending_.emplace(id, Pending{vcpu, read});
        state.outstanding = true;
    }

    const Pending* pending(uint64_t id) const {
        auto it = pending_.find(id);
        return it == pending_.end() ? nullptr : &it->second;
    }

    void complete(uint64_t id, uint32_t vcpu, bool read,
                  const std::vector<uint8_t>& bytes = {}) {
        auto it = pending_.find(id);
        if (it == pending_.end() || it->second.vcpu != vcpu || it->second.read != read)
            throw std::logic_error("window cache response ownership/type mismatch");
        auto& state = states_.at(vcpu);
        state.cache.complete(bytes);
        state.outstanding = false;
        pending_.erase(it);
    }

    void resetCore(uint32_t vcpu) {
        auto& state = states_.at(vcpu);
        if (state.outstanding || state.cache.active())
            throw std::logic_error("window cache reset during an outstanding transaction");
        state.cache.reset();
    }

private:
    struct State { WindowDataCache cache; bool outstanding = false; };
    std::vector<State> states_;
    std::unordered_map<uint64_t, Pending> pending_;
};

} }
#endif
