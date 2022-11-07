"""Test hook that injects delays into server-side Hello Cmd handler."""

from __future__ import absolute_import

import os

from buildscripts.resmokelib import errors
from buildscripts.resmokelib import utils
from buildscripts.resmokelib.testing.hooks import interface
from buildscripts.resmokelib.testing.fixtures import replicaset
from buildscripts.resmokelib.testing.fixtures import shardedcluster

from . import interface
from . import jsfile


class HelloDelays(interface.Hook):
    """Sets Hello fault injections."""

    def __init__(self, hook_logger, fixture):
        """Initialize HelloDelays."""
        description = "Sets Hello fault injections"
        interface.Hook.__init__(self, hook_logger, fixture, description)
        self.js_filename = os.path.join("jstests", "hooks", "run_inject_hello_failures.js")
        self.cleanup_js_filename = os.path.join("jstests", "hooks", "run_cleanup_hello_failures.js")
        self.shell_options = None

    def before_test(self, test, test_report):
        """Each test will call this before it executes."""
        print 'before_test hook starts injecting Hello failures'
        hook_test_case = jsfile.DynamicJSTestCase.create_before_test(
            self.logger.test_case_logger, test, self, self.js_filename, self.shell_options)
        hook_test_case.configure(self.fixture)
        hook_test_case.run_dynamic_test(test_report)

    def after_test(self, test, test_report):
        """Each test will call this after it executes."""
        print 'Cleanup hook is starting to remove Hello fail injections'
        hook_test_case = jsfile.DynamicJSTestCase.create_after_test(
            self.logger.test_case_logger, test, self, self.cleanup_js_filename, self.shell_options)
        hook_test_case.configure(self.fixture)
        hook_test_case.run_dynamic_test(test_report)
