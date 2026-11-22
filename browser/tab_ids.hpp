// SPDX-License-Identifier: GPL-3.0-or-later
// Tab ids come from two counters: the UI's (1, 2, ...) and the engine's for pages that open windows (it starts at
// WPE_ENGINE_TAB_ID_BASE again at every engine start). The UI keeps its tabs when the engine dies, so ids of popups from
// the old engine would clash with those of the new one's. When the engine is gone, every tab that has such an id is
// given a fresh UI id.
#pragma once

#include <cstdint>
#include <vector>

namespace browser {

class TabIdRemap {
 public:
    // Gives each id in `ids` that is >= `engine_base` the next id from `next_id` (which advances).
    TabIdRemap(const std::vector<uint32_t> &ids, uint32_t engine_base, uint32_t &next_id) : base_(engine_base) {
        for (uint32_t id : ids)
            if (id >= engine_base) moved_.push_back({id, next_id++});
    }
    bool empty() const { return moved_.empty(); }
    // The id to use now for one that was held before: the new id of a tab that moved; 0 for an engine id no tab has any more
    // (it names nothing, and must not name a popup of the next engine); anything else unchanged.
    uint32_t operator()(uint32_t id) const {
        for (const auto &m : moved_)
            if (m.first == id) return m.second;
        return id >= base_ ? 0 : id;
    }

 private:
    uint32_t base_;
    std::vector<std::pair<uint32_t, uint32_t>> moved_;
};

}  // namespace browser
