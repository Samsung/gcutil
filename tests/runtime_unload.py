#!/usr/bin/env python3
# Copyright (c) 2026 Samsung Electronics Co., Ltd. All rights reserved.
"""Exercise final collector shutdown with a live host thread across dlclose."""
import _ctypes
import ctypes
import os
from pathlib import Path
import resource
import signal
import sys


def next_key():
    key = ctypes.c_uint()
    assert libc.pthread_key_create(ctypes.byref(key), None) == 0
    assert libc.pthread_key_delete(key.value) == 0
    return key.value


def descriptors():
    result = {}
    for entry in Path("/proc/self/fd").iterdir():
        try:
            result[entry.name] = str(entry.readlink())
        except FileNotFoundError:
            pass  # The descriptor used by iterdir() has already been closed.
    return result


path = str(Path(sys.argv[1]).resolve())
libc = ctypes.CDLL(None)
initial_key = next_key()
initial_fds = descriptors()
initial_threads = set(Path("/proc/self/task").iterdir())
vdb_modes = set()
for cycle in range(40):
    library = ctypes.CDLL(path, mode=os.RTLD_NOW | os.RTLD_LOCAL)
    initialize = library.runtime_unload_initialize
    initialize.argtypes = [ctypes.c_int]
    initialize.restype = ctypes.c_uint
    destroy = library.runtime_unload_destroy
    destroy.restype = None
    mode = initialize(cycle % 2)
    assert cycle % 2 == 0 or mode != 0, "incremental GC was not exercised"
    vdb_modes.add(mode)
    destroy()
    if cycle == 0:
        child = os.fork()
        if child == 0:
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
            fd = os.open(os.devnull, os.O_WRONLY)
            os.dup2(fd, 2)
            initialize(0)
            os._exit(0)
        _, status = os.waitpid(child, 0)
        assert os.WIFSIGNALED(status) and os.WTERMSIG(status) == signal.SIGABRT
    _ctypes.dlclose(library._handle)
    assert path not in Path("/proc/self/maps").read_text(), "DSO still mapped"
    assert next_key() == initial_key, "native TLS key leaked"
    assert descriptors() == initial_fds, "file descriptor leaked"
    assert set(Path("/proc/self/task").iterdir()) == initial_threads, "thread leaked"
print("PASS: 40 collector unload/reload cycles; VDB modes:", sorted(vdb_modes))
