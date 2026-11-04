// Source-only filesystem fixtures have no calendar store. Explicit definitions
// also satisfy sanitizer-generated references to weak-imported functions.
// A fixture linking a real provider overrides these weak definitions normally.
#ifndef DM_TEST_BCAT_ABSENT_H
#define DM_TEST_BCAT_ABSENT_H
#include <stddef.h>
__attribute__((weak)) const char *dm_bcat_overlay(void) { return NULL; }
__attribute__((weak)) const char *dm_bcat_delivery(void) { return NULL; }
__attribute__((weak)) int dm_bcat_copy_delivery(char *out, size_t size) { (void)out; (void)size; return 0; }
#endif
