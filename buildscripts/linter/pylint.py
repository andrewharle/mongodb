"""PyLint linter support module."""
from __future__ import absolute_import
from __future__ import print_function

import os
from typing import List

from . import base
from . import git


class PyLintLinter(base.LinterBase):
    """Pylint linter."""

    def __init__(self):
        # type: () -> None
        """Create a pylint linter."""
        super(PyLintLinter, self).__init__("pylint", "pylint 1.8.3")

    def get_lint_version_cmd_args(self):
        # type: () -> List[str]
        """Get the command to run a linter version check."""
        return ["--version"]

    def get_lint_cmd_args(self, file_name):
        # type: (str) -> List[str]
        """Get the command to run a linter."""
        return ["--output-format=msvs", "--reports=n", file_name]
