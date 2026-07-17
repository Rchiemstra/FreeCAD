"""Exit codes and exception types for freecad-git."""

from __future__ import annotations

# Documented stable exit codes
EXIT_SUCCESS = 0
EXIT_STALE_OR_MISSING = 1
EXIT_UNSAFE_ARCHIVE = 2
EXIT_INVALID_XML = 3
EXIT_INVALID_SCHEMA = 4
EXIT_INVALID_CONFIG = 5
EXIT_UNSUPPORTED_DOCUMENT = 6
EXIT_DIAGNOSTIC_FAILURE = 7
EXIT_IO_ERROR = 8
EXIT_GENERAL_FAILURE = 9


class FreecadGitError(Exception):
    """Base error with an associated exit code."""

    exit_code: int = EXIT_GENERAL_FAILURE

    def __init__(self, message: str) -> None:
        super().__init__(message)
        self.message = message


class UnsafeArchiveError(FreecadGitError):
    exit_code = EXIT_UNSAFE_ARCHIVE


class InvalidXmlError(FreecadGitError):
    exit_code = EXIT_INVALID_XML


class InvalidSchemaError(FreecadGitError):
    exit_code = EXIT_INVALID_SCHEMA


class InvalidConfigError(FreecadGitError):
    exit_code = EXIT_INVALID_CONFIG


class UnsupportedDocumentError(FreecadGitError):
    exit_code = EXIT_UNSUPPORTED_DOCUMENT


class DiagnosticFailureError(FreecadGitError):
    exit_code = EXIT_DIAGNOSTIC_FAILURE


class IOError(FreecadGitError):
    exit_code = EXIT_IO_ERROR


class StaleSidecarError(FreecadGitError):
    exit_code = EXIT_STALE_OR_MISSING


class MissingSidecarError(FreecadGitError):
    exit_code = EXIT_STALE_OR_MISSING


class MalformedSidecarError(FreecadGitError):
    exit_code = EXIT_INVALID_SCHEMA
