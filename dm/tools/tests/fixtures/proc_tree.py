"""A process tree for test_proc_group.py: this wrapper starts a child in its own process group (the group the supervisor under test owns); the child writes its pid to
the marker file and only then tells the wrapper it is ready (a byte on a pipe), and only then the wrapper prints "READY <wrapper pid> <child pid>" (and writes the same line
to --fifo when given, for a caller whose output goes to a file). No timing: each side waits for the other's word.

  proc_tree.py MARKER [--fifo PATH] [--child-out pipe|null] [--child SIGNALS] [--wrapper wait|exit:N|join:N]

--child-out: the child keeps the wrapper's stdout (pipe, the default) or writes to /dev/null.
--child: what the child does with SIGINT and SIGTERM: mark (the default: appends INT or TERM to the marker and exits), ignore (both ignored), or exit (it exits as soon
  as it is ready; with cont, mark also exits on SIGCONT, appending CONT). After that it waits for signals.
--wrapper: after READY, wait for signals (the default; SIGINT and SIGTERM end it), exit:N (exit with N at once, the child left running) or join:N (wait for the child, then
  print "done" and exit with N)."""
import argparse, os, signal, subprocess, sys


def child(a, ready):
    def mark(sig, frame):
        with open(a.marker, 'a') as f: f.write(f'{signal.Signals(sig).name[3:]}\n')
        os._exit(0)
    if a.child in ('mark', 'mark+cont'):
        signal.signal(signal.SIGINT, mark); signal.signal(signal.SIGTERM, mark)
        if a.child == 'mark+cont': signal.signal(signal.SIGCONT, mark)
    elif a.child == 'ignore':
        signal.signal(signal.SIGINT, signal.SIG_IGN); signal.signal(signal.SIGTERM, signal.SIG_IGN)
    with open(a.marker, 'w') as f: f.write(f'{os.getpid()}\n')
    os.write(ready, b'1'); os.close(ready)
    if a.child == 'exit': return
    while True: signal.pause()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('marker'); ap.add_argument('--fifo'); ap.add_argument('--child-out', default='pipe', choices=('pipe', 'null'))
    ap.add_argument('--child', default='mark', choices=('mark', 'mark+cont', 'ignore', 'exit')); ap.add_argument('--wrapper', default='wait')
    ap.add_argument('--as-child', type=int, help=argparse.SUPPRESS)
    a = ap.parse_args()
    if a.as_child is not None: return child(a, a.as_child)
    r, w = os.pipe()
    argv = [sys.executable, __file__, a.marker, '--child', a.child, '--as-child', str(w)]
    out = subprocess.DEVNULL if a.child_out == 'null' else None
    c = subprocess.Popen(argv, stdout=out, stderr=out, pass_fds=(w,))
    os.close(w)
    if os.read(r, 1) != b'1': sys.exit('the child did not get ready')
    line = f'READY {os.getpid()} {c.pid}'
    print(line, flush=True)
    if a.fifo:
        with open(a.fifo, 'w') as f: f.write(line + '\n')
    mode, _, n = a.wrapper.partition(':')
    if mode == 'exit': os._exit(int(n))
    if mode == 'join':
        c.wait(); print('done', flush=True); sys.exit(int(n))
    while True: signal.pause()


if __name__ == '__main__':
    main()
