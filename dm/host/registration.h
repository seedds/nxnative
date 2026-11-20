// Linker-collected registrations are walked as arrays of their entry type.
#pragma once

// Every producer in a section uses the same type. Keep its natural alignment:
// stronger per-global alignment or ASan redzones would insert gaps in the array.
// Suppress only the entry's redzones; its functions and pointed-to data stay checked.
#define DM_REGISTER(sect, type, name, ...) \
    __attribute__((used, section("__DATA," sect), aligned(_Alignof(type)), no_sanitize("address"))) \
    static const type name = __VA_ARGS__
