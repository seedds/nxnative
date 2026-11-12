"""A subprocess in a process group of its own, which the caller owns with every process it starts (the NSP script's run, regress.py's run_heavy).

The caller's interrupt does not reach that group by itself: Spyder's interrupt goes to its kernel alone, a front end's SIGTERM to the script alone, and a terminal's Ctrl-C to
the terminal's foreground group, which this group is not in (a session of its own: no signal from the terminal reaches it, and a member that reads the terminal fails at once
instead of stopping on SIGTTIN). So the group is stopped here:

- on an exception (an interrupt, a SIGTERM the caller turned into one, an error such as a log that cannot be written): stop() sends the group the signal that stopped the caller
  (SIGINT for an interrupt, else SIGTERM), after a SIGCONT for a group a pause left stopped, gives it `grace` seconds, then SIGKILLs what is left and reaps the direct child;
  an interrupt during that wait only shortens it to the SIGKILL;
- after the direct child's normal exit: finish() gives the members left (a descendant still writing, or holding the output pipe) `drain` seconds, then stops them the same
  way and lists them in `stragglers`, which the caller reports as a failure of its stage: their outputs may be incomplete.

A process that leaves the group itself (setsid, setpgid) is out of reach of group supervision: it is not listed, signalled or waited for (only a holder of the output is
seen, by lines(), and fails the step unnamed)."""
import codecs, ctypes, io, locale, os, selectors, signal, subprocess, sys, time

GRACE = 5.0   # seconds the group has to end after the stopping signal, before SIGKILL
DRAIN = 5.0   # seconds members may outlive the direct child's normal exit
REAP = 2.0    # seconds for the killed members to go (launchd reaps the orphans among them)
TICK = 0.05   # seconds between looks at the group while waiting for it to end

_libproc = ctypes.CDLL('/usr/lib/libproc.dylib', use_errno=True) if sys.platform == 'darwin' else None
if _libproc is not None:
    _libproc.proc_listpids.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_int]
    _libproc.proc_pidinfo.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_int]


def members(pgid):
    """[(pid, name)] of the live processes in process group pgid; zombies are not members (they write nothing; the caller or launchd reaps them)"""
    if _libproc is None:   # another system: only whether the group has a member, no names
        try: os.killpg(pgid, 0); return [(pgid, '?')]
        except ProcessLookupError: return []
        except PermissionError: return [(pgid, '?')]
    buf = (ctypes.c_int * 1024)()
    n = _libproc.proc_listpids(2, pgid, buf, ctypes.sizeof(buf)) // ctypes.sizeof(ctypes.c_int)   # PROC_PGRP_ONLY
    out, info = [], ctypes.create_string_buffer(136)   # struct proc_bsdinfo
    for pid in buf[:max(n, 0)]:
        if pid <= 0: continue
        if _libproc.proc_pidinfo(pid, 3, 0, info, 136) != 136: continue   # PROC_PIDTBSDINFO; fails for a zombie or a process gone meanwhile
        status, pg = int.from_bytes(info.raw[4:8], 'little'), int.from_bytes(info.raw[100:104], 'little')
        if status == 5 or pg != pgid: continue   # SZOMB; a pid reused outside the group
        out.append((pid, info.raw[64:96].split(b'\0')[0].decode(errors='replace') or info.raw[48:64].split(b'\0')[0].decode(errors='replace')))
    return out


class Group:
    """The direct child (p, a subprocess.Popen started with these Popen arguments; for lines(), stdout=PIPE in binary mode) leads a new session and process group.
    `with Group(...) as g:` stops the group when the block ends by an exception, and finishes it (finish) when it ends otherwise.
    stragglers: [(pid, name)] that outlived the direct child's normal exit and were stopped (pid None: a holder of the output outside the group), for the caller to report"""

    def __init__(self, cmd, grace=None, drain=None, **popen):   # grace, drain: GRACE and DRAIN when None
        self.grace, self.drain = GRACE if grace is None else grace, DRAIN if drain is None else drain
        self.paused, self.stragglers, self.done = False, [], False
        self.p = subprocess.Popen([str(c) for c in cmd], start_new_session=True, **popen)
        self.pgid = self.p.pid

    def __enter__(self): return self

    def __exit__(self, kind, exc, tb):
        try:
            if kind is not None: self.stop(signal.SIGINT if issubclass(kind, KeyboardInterrupt) else signal.SIGTERM)
            elif not self.done: self.finish()   # a caller that did not wait: the same end, its stragglers in the list
        finally:
            if self.p.stdout: self.p.stdout.close()   # after the stop: a member is not cut off by SIGPIPE before its signal
        return False

    def _signal(self, sig):
        if self.p.returncode is not None and not members(self.pgid): return   # its leader reaped and its members gone: the number may be another group's now
        try: os.killpg(self.pgid, sig)
        except (ProcessLookupError, PermissionError): pass   # every member gone, or only zombies left (macOS answers EPERM for those)

    def pause(self): self._signal(signal.SIGSTOP); self.paused = True

    def resume(self): self._signal(signal.SIGCONT); self.paused = False

    def _gone(self, secs):   # waits up to secs for the group to end (reaping the direct child on the way); → whether it did
        end = time.monotonic() + secs
        while True:
            self.p.poll()
            if not members(self.pgid): return True
            if time.monotonic() >= end: return False
            time.sleep(TICK)

    def stop(self, sig=signal.SIGTERM):
        """sig to the group (resumed first: a stopped process acts on no signal but SIGKILL), up to grace seconds, then SIGKILL; returns once the direct child is reaped and
        no member is left, or REAP seconds after the SIGKILL (a member stuck in the kernel). Re-raises the last interrupt (or SIGTERM, exit) that came while it waited"""
        late, self.done = None, True
        try:
            self.resume(); self._signal(sig)
            if self._gone(self.grace): self.p.wait(); return
        except BaseException as e: late = e   # a second interrupt or SIGTERM: no more grace
        while True:   # each step can be repeated; an interrupt here only repeats it
            try:
                self._signal(signal.SIGKILL); self.p.wait(); self._gone(REAP); break
            except BaseException as e: late = e
        if late is not None: raise late

    def finish(self):
        """After the direct child's exit (waits for it): drain seconds for the other members to go, then stop() those left, listed in stragglers. → the child's exit status"""
        self.p.wait(); self.done = True
        if not self._gone(self.drain):
            self.stragglers += members(self.pgid); self.stop()
        return self.p.returncode

    def lines(self):
        """The output of a group started with stdout=PIPE, a line at a time as text (universal newlines, undecodable bytes replaced: as text=True reads it). Ends at the end of
        the output, or drain seconds after the direct child's exit while a member still holds the pipe: those members are stopped then (stragglers) and the end follows.
        The direct child's exit is seen whether or not output comes; finish() then waits for the rest of the group"""
        out = self.p.stdout
        assert out is not None, 'lines() reads a group started with stdout=PIPE'
        fd = out.fileno()
        dec = io.IncrementalNewlineDecoder(codecs.getincrementaldecoder(locale.getpreferredencoding(False))(errors='replace'), translate=True)
        sel, buf, end, stopped = selectors.DefaultSelector(), '', None, False
        sel.register(fd, selectors.EVENT_READ)
        try:
            while True:
                if sel.select(0.1):
                    data = os.read(fd, 65536)
                    if not data: break
                    buf += dec.decode(data)
                    *done, buf = buf.split('\n')
                    for line in done: yield line + '\n'
                if end is None and self.p.poll() is not None: end = time.monotonic() + self.drain
                if end is not None and time.monotonic() >= end:
                    self.stragglers += members(self.pgid) if not stopped else []
                    if stopped or not self.stragglers:   # a holder outside the group (it left it): stop reading, and fail the stage all the same
                        self.stragglers.append((None, 'a process outside the group holding its output')); break
                    self.stop(); stopped, end = True, time.monotonic() + REAP   # the pipe closes as they go
        finally:
            sel.close()
        buf += dec.decode(b'', final=True)
        if buf: yield buf


def describe(stragglers):   # for the caller's message
    return ', '.join(name if pid is None else f'{name} (pid {pid})' for pid, name in stragglers)
