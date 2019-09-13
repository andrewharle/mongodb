"""Test hook for verifying members of a replica set have matching oplogs."""

from __future__ import absolute_import

import os.path

from . import jsfile


class CheckReplOplogs(jsfile.DataConsistencyHook):  # pylint: disable=non-parent-init-called,super-init-not-called
    """Check that local.oplog.rs matches on the primary and secondaries."""

    def __init__(  # pylint: disable=super-init-not-called
            self, hook_logger, fixture, shell_options=None):
        """Initialize CheckReplOplogs."""
        description = "Check oplogs of all replica set members"
        js_filename = os.path.join("jstests", "hooks", "run_check_repl_oplogs.js")
        jsfile.JSHook.__init__(  # pylint: disable=non-parent-init-called
            self, hook_logger, fixture, js_filename, description, shell_options=shell_options)
