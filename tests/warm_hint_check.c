// runtime/vk_warm_hint.h: what the library warm's log adds when the driver's cache of stage libraries is cold
// (runtime/vk_pipeline.c lw_main), by GPU vendor and by why it is cold. Built and run by tests/warm_hint_check.py;
// prints each failed check and exits 1 on any.
#include <stdio.h>
#include <string.h>

#include "vk_warm_hint.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { NVIDIA = 0x10de, AMD = 0x1002, INTEL = 0x8086 };

static int has(const char *s, const char *part) { return s && strstr(s, part) != NULL; }

int main(void) {
    // Cold at a start with no finished warm before it (a driver update, or the build's preparation step, which writes
    // no shader-warm.txt): on NVIDIA the driver's Shader Cache Size limit is named as another cause, with the setting.
    const char *first = lw_cold_hint(NVIDIA, LW_COLD_FIRST);
    CHECK(has(first, "Shader Cache Size") && has(first, "Unlimited"), "NVIDIA first cold: %s", first);
    CHECK(first && first[0] == ' ', "a hint starts with a space, as it follows a sentence: \"%s\"", first);
    CHECK(first && !strcmp(lw_cold_hint(AMD, LW_COLD_FIRST), ""), "AMD first cold: no NVIDIA setting");
    CHECK(first && !strcmp(lw_cold_hint(INTEL, LW_COLD_FIRST), ""), "Intel first cold: no NVIDIA setting");

    // Cold right after a finished warm on the same driver: on NVIDIA that is the cache size limit, said plainly.
    const char *again = lw_cold_hint(NVIDIA, LW_COLD_AFTER_WARM);
    CHECK(has(again, "Shader Cache Size") && has(again, "Unlimited"), "NVIDIA cold after a warm: %s", again);
    CHECK(strcmp(again, first), "cold after a warm says more than a first cold");
    CHECK(!strcmp(lw_cold_hint(AMD, LW_COLD_AFTER_WARM), ""), "AMD cold after a warm: no NVIDIA setting");

    // Warming given up after two cold starts in a row: every vendor is told how to warm again once the cache can
    // hold the libraries; NVIDIA also names the setting that lets it.
    for (unsigned v = 0; v < 3; v++) {
        unsigned vendor = v == 0 ? NVIDIA : v == 1 ? AMD : INTEL;
        const char *gave = lw_cold_hint(vendor, LW_COLD_GAVE_UP);
        CHECK(has(gave, "lib_warm = force") && has(gave, "shader-warm.txt"), "vendor %04x gave up: %s", vendor, gave);
    }
    CHECK(has(lw_cold_hint(NVIDIA, LW_COLD_GAVE_UP), "Shader Cache Size"), "NVIDIA gave up: names the setting");
    CHECK(!has(lw_cold_hint(AMD, LW_COLD_GAVE_UP), "Shader Cache Size"), "AMD gave up: no NVIDIA setting");

    if (!fails) printf("warm hint: all checks passed\n");
    return fails != 0;
}
