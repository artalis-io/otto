"""Surge solver — ctypes wrapper around libsurge_api shared library."""

import ctypes
import json
import os
from pathlib import Path

from .exceptions import SolverError, SurgeError, ValidationError

_lib = None
_libc = None


def _find_library():
    """Find the surge shared library.

    Search order:
      1. SURGE_LIB_PATH environment variable
      2. Package-relative (installed package with bundled .so/.dylib)
      3. Build-dir-relative (development: surge/python/surge -> surge/)
      4. System library path
    """
    names = ("libsurge_api.dylib", "libsurge_api.so")

    # 1. Explicit env var
    env_path = os.environ.get("SURGE_LIB_PATH")
    if env_path and os.path.isfile(env_path):
        return env_path

    # 2. Package-relative
    pkg_dir = Path(__file__).parent
    for name in names:
        p = pkg_dir / name
        if p.is_file():
            return str(p)

    # 3. Build-dir-relative (surge/bindings/python/surge -> surge/)
    build_dir = pkg_dir.parent.parent.parent
    for name in names:
        p = build_dir / name
        if p.is_file():
            return str(p)

    # 4. System library path
    for name in names:
        try:
            ctypes.CDLL(name)
            return name
        except OSError:
            continue

    raise SurgeError(
        "Could not find libsurge_api shared library. "
        "Set SURGE_LIB_PATH or run 'make shared-lib' in the surge directory."
    )


def _load():
    """Lazy-load the shared library on first call."""
    global _lib, _libc
    if _lib is not None:
        return

    path = _find_library()
    _lib = ctypes.CDLL(path)

    # char *sg_api_solve(const char *json_body, size_t body_len,
    #                    int *status_code, size_t *out_len)
    _lib.sg_api_solve.argtypes = [
        ctypes.c_char_p,
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_size_t),
    ]
    _lib.sg_api_solve.restype = ctypes.c_void_p  # raw pointer for free()

    # char *sg_api_version(size_t *out_len)
    _lib.sg_api_version.argtypes = [ctypes.POINTER(ctypes.c_size_t)]
    _lib.sg_api_version.restype = ctypes.c_void_p

    # char *sg_api_health(size_t *out_len)
    _lib.sg_api_health.argtypes = [ctypes.POINTER(ctypes.c_size_t)]
    _lib.sg_api_health.restype = ctypes.c_void_p

    # libc free() for releasing allocated buffers
    _libc = ctypes.CDLL(None)
    _libc.free.argtypes = [ctypes.c_void_p]
    _libc.free.restype = None


def solve(problem: dict) -> dict:
    """Solve a vehicle routing problem.

    Args:
        problem: Dict with VRP problem definition
                 (depots, vehicles, tasks, requests, config, etc.)

    Returns:
        Dict with solution (status, stats, routes, unassigned).

    Raises:
        ValidationError: Invalid problem definition (HTTP 400).
        SolverError: Internal solver error (HTTP 500).
    """
    _load()

    body = json.dumps(problem, separators=(",", ":")).encode("utf-8")
    status_code = ctypes.c_int(0)
    out_len = ctypes.c_size_t(0)

    ptr = _lib.sg_api_solve(
        body, len(body), ctypes.byref(status_code), ctypes.byref(out_len)
    )

    if not ptr:
        raise SolverError("sg_api_solve returned NULL")

    try:
        result_bytes = ctypes.string_at(ptr, out_len.value)
        result = json.loads(result_bytes)
    finally:
        _libc.free(ptr)

    code = status_code.value
    if code == 400:
        raise ValidationError(result.get("error", "Validation error"))
    elif code >= 500:
        raise SolverError(result.get("error", "Solver error"))

    return result


def version() -> str:
    """Get the Surge solver version string."""
    _load()
    out_len = ctypes.c_size_t(0)
    ptr = _lib.sg_api_version(ctypes.byref(out_len))
    if not ptr:
        raise SurgeError("sg_api_version returned NULL")
    try:
        result_bytes = ctypes.string_at(ptr, out_len.value)
        return json.loads(result_bytes).get("version", "")
    finally:
        _libc.free(ptr)


def health() -> dict:
    """Get the Surge solver health status."""
    _load()
    out_len = ctypes.c_size_t(0)
    ptr = _lib.sg_api_health(ctypes.byref(out_len))
    if not ptr:
        raise SurgeError("sg_api_health returned NULL")
    try:
        result_bytes = ctypes.string_at(ptr, out_len.value)
        return json.loads(result_bytes)
    finally:
        _libc.free(ptr)
