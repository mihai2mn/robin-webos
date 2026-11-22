// SPDX-License-Identifier: GPL-3.0-or-later
// Tabs that carry an id from a dead engine get fresh UI ids, so the next engine's popup ids cannot clash with them.
#include "tab_ids.hpp"

#include <iostream>
#include <string>

static int failures = 0;
static void check(bool ok, const std::string &what) {
    std::cout << (ok ? "ok   " : "FAIL ") << what << '\n';
    if (!ok) ++failures;
}

int main() {
    const uint32_t base = 0x40000000u;
    uint32_t next_id = 5;
    const browser::TabIdRemap r({1, 2, base, base + 1}, base, next_id);
    check(!r.empty(), "popup ids move");
    check(r(1) == 1 && r(2) == 2, "UI ids stay");
    check(r(base) == 5 && r(base + 1) == 6, "popup ids get the next UI ids, in order");
    check(next_id == 7, "the UI counter advanced past them");
    check(r(base + 9) == 0, "an engine id no tab has any more becomes 0");
    check(r(0) == 0, "0 stays 0");
    check(r(5) == 5, "a UI id equal to a new id is not re-mapped");

    uint32_t next2 = 3;
    const browser::TabIdRemap none({1, 2}, base, next2);
    check(none.empty() && next2 == 3 && none(2) == 2, "no popup tabs: nothing changes");

    // Two engine lives: the second engine's first popup is base again; it no longer clashes with the renumbered tab.
    check(r(base) != base, "the old popup no longer has the id the next engine hands out first");
    return failures == 0 ? 0 : 1;
}
