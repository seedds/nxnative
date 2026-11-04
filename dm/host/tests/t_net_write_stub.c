// Ordinary-buffer network fixtures have no guest dirty tracker. The dedicated
// t_net_write_prepare fixture supplies a dispatcher into the real tracker.
#include <stddef.h>
void dm_guest_write_prepare(void *address, size_t size) { (void)address; (void)size; }
