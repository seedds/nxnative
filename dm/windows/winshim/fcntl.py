# Windows has no fcntl. The build runs alone in this folder, so its folder locks have nothing to wait for
LOCK_EX, LOCK_NB, LOCK_SH, LOCK_UN = 2, 4, 1, 8
def flock(fd, op): return None
