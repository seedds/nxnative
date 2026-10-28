# Starting the shader tools cheaply. A shader build starts several processes per shader (translator, spirv-cross, metal, metallib), 300,000 and more for a game of 70,000 shaders.
#   · posix_spawn instead of fork: subprocess forks the whole Python process unless close_fds is False and the program is a path, and a fork costs in proportion to the
#     process's memory and is not run in parallel (Python 3.9 on an M4 Pro: 15 ms a start at 1 GB, 39 ms at 3 GB, against 1.3 ms for posix_spawn at any size; dmsh_build.py
#     holds every shader package in memory). close_fds=False passes on nothing extra: Python opens its descriptors non-inheritable (PEP 446), and the pipes it hands a child
#     are moved onto 0 / 1 / 2. So run() starts the program by its resolved path with close_fds=False
#   · no xcrun on every call: `xcrun -sdk <sdk> <tool> ...` runs the tool's resolved path directly with the environment xcrun gives it (SDKROOT; it also sets MANPATH, which
#     no tool reads). xcrun costs about 5 ms a call with its cache warm, 70 ms or more when cold. Without SDKROOT metal writes a different library; with it the output is byte-identical
# Not part of the translation chain version (dmsh_chain.py): it changes how a tool is started, never what it writes.
import os, shutil, threading, subprocess

_resolved, _lock = {}, threading.Lock()


def _xcrun_tool(sdk, tool):   # → (path, environment) for `xcrun -sdk <sdk> <tool>`
    with _lock:
        if (sdk, tool) not in _resolved:
            def ask(*a): return subprocess.run(['xcrun', '-sdk', sdk, *a], capture_output=True, text=True, check=True).stdout.strip()
            _resolved[sdk, tool] = (ask('-f', tool), {**os.environ, 'SDKROOT': ask('--show-sdk-path')})
        return _resolved[sdk, tool]


def _path(prog):   # a program name → its path on PATH (unchanged when it has a directory or is not found: subprocess then reports it as before)
    if os.path.dirname(prog): return prog
    with _lock:
        if prog not in _resolved: _resolved[prog] = shutil.which(prog) or prog
        return _resolved[prog]


def run(argv, **kw):
    """subprocess.run(argv, **kw), started with posix_spawn, and `xcrun -sdk <sdk> <tool> ...` without xcrun"""
    argv = [str(a) for a in argv]
    if len(argv) > 3 and argv[:2] == ['xcrun', '-sdk'] and not argv[3].startswith('-') and 'env' not in kw:
        path, kw['env'] = _xcrun_tool(argv[2], argv[3])
        argv = [path, *argv[4:]]
    else: argv[0] = _path(argv[0])
    kw.setdefault('close_fds', False)
    return subprocess.run(argv, **kw)
