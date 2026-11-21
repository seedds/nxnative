# Standalone Metal probes (no game): see README.md. Each is one Objective-C file; `make` builds them all, `make run` runs those that need no input.
CC = clang
CFLAGS = -O1 -g -Wall -fobjc-arc
FW = -framework Metal -framework Foundation -framework QuartzCore
PROBES = heap_alias raw_sites mesh_index_range mesh_archive rt_sizes mesh_sweep task_hang
all: $(PROBES)
%: %.m
	$(CC) $(CFLAGS) -o $@ $< $(FW)
run: heap_alias mesh_index_range mesh_archive rt_sizes
	./heap_alias
	for m in a b c; do ./mesh_index_range $$m 1000; done
	./mesh_archive
	./rt_sizes 2048 1152
clean:
	rm -rf $(PROBES) *.dSYM
.PHONY: all run clean
