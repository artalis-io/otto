"""Surge solver exceptions."""


class SurgeError(Exception):
    """Base exception for Surge solver errors (e.g. library load failures)."""


class ValidationError(SurgeError):
    """Invalid problem definition (HTTP 400)."""


class SolverError(SurgeError):
    """Internal solver error (HTTP 500)."""
