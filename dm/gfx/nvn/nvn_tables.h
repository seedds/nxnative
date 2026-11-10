// Which copy of a library's table to take (pure; unit test tests/t_tables.c). c1_full.tsv and cb_extent.tsv are made from one library's Mac outputs and name its chain and inputs in
// their first line (dmsh_chain.py table_head); the host takes a table only when that line names the library it loads. The profile folder beside the shader map holds one copy, which
// fits one library: a host pointed (DM_MSL_DIR) at another library of the same game, another chain or another build of the profile, found the profile's copy stale, and every c1 got
// 64 KiB of its own (PRAGMATA r11: 779 shaders on the title route, about 48 MiB where the shared banks take about 5) and every uniform binding was copied whole.
// A copy beside the library, <library>.<table> (b-<chain>.c1_full.tsv next to b-<chain>.img), is taken when the profile's is not that library's; the profile's wins when both are.
#pragma once
#include <string.h>

// heads[i]: the first line of candidate i (NULL when it is missing); want: the prefix that names the library ("# chain b-<V> inputs <digest> "). The first match, else -1
static inline int nvn_table_pick(const char *const *heads, int n, const char *want) {
    if (!want || !*want) return -1;
    for (int i = 0; i < n; i++) if (heads[i] && !strncmp(heads[i], want, strlen(want))) return i;
    return -1;
}
