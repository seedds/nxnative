#pragma once

// Call after the launcher's final save/cloud environment is set, before the host runs.
// executable is the path to this same executable. Returns 0 (also when disabled), or errno.
int dm_save_cleanup_start(const char *executable, const char *title);

// Must be the first branch in main, before UI, dylib loading or log rotation.
// Returns -1 for ordinary invocation, otherwise the helper's exit status.
int dm_save_cleanup_main(int argc, char **argv);
