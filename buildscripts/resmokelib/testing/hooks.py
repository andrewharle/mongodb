"""
Customize the behavior of a fixture by allowing special code to be
executed before or after each test, and before or after each suite.
"""

from __future__ import absolute_import

import os
import sys

import bson
import pymongo
import random

from . import fixtures
from . import testcases
from .. import errors
from .. import logging
from .. import utils


def make_custom_behavior(class_name, *args, **kwargs):
    """
    Factory function for creating CustomBehavior instances.
    """

    if class_name not in _CUSTOM_BEHAVIORS:
        raise ValueError("Unknown custom behavior class '%s'" % (class_name))
    return _CUSTOM_BEHAVIORS[class_name](*args, **kwargs)


class CustomBehavior(object):
    """
    The common interface all CustomBehaviors will inherit from.
    """

    @staticmethod
    def start_dynamic_test(hook_test_case, test_report):
        """
        If a CustomBehavior wants to add a test case that will show up
        in the test report, it should use this method to add it to the
        report, since we will need to count it as a dynamic test to get
        the stats in the summary information right.
        """
        test_report.startTest(hook_test_case, dynamic=True)

    def __init__(self, logger, fixture, description):
        """
        Initializes the CustomBehavior with the specified fixture.
        """

        if not isinstance(logger, logging.Logger):
            raise TypeError("logger must be a Logger instance")

        self.logger = logger
        self.fixture = fixture
        self.hook_test_case = None
        self.logger_name = self.__class__.__name__
        self.description = description


    def before_suite(self, test_report):
        """
        The test runner calls this exactly once before they start
        running the suite.
        """
        pass

    def after_suite(self, test_report):
        """
        The test runner calls this exactly once after all tests have
        finished executing. Be sure to reset the behavior back to its
        original state so that it can be run again.
        """
        pass

    def before_test(self, test, test_report):
        """
        Each test will call this before it executes.

        Raises a TestFailure if the test should be marked as a failure,
        or a ServerFailure if the fixture exits uncleanly or
        unexpectedly.
        """
        pass

    def after_test(self, test, test_report):
        """
        Each test will call this after it executes.

        Raises a TestFailure if the test should be marked as a failure,
        or a ServerFailure if the fixture exits uncleanly or
        unexpectedly.
        """
        pass


class CleanEveryN(CustomBehavior):
    """
    Restarts the fixture after it has ran 'n' tests.
    On mongod-related fixtures, this will clear the dbpath.
    """

    DEFAULT_N = 20

    def __init__(self, logger, fixture, n=DEFAULT_N):
        description = "CleanEveryN (restarts the fixture after running `n` tests)"
        CustomBehavior.__init__(self, logger, fixture, description)

        # Try to isolate what test triggers the leak by restarting the fixture each time.
        if "detect_leaks=1" in os.getenv("ASAN_OPTIONS", ""):
            self.logger.info("ASAN_OPTIONS environment variable set to detect leaks, so restarting"
                             " the fixture after each test instead of after every %d.", n)
            n = 1

        self.n = n
        self.tests_run = 0

    def after_test(self, test, test_report):
        self.tests_run += 1
        if self.tests_run >= self.n:
            self.logger.info("%d tests have been run against the fixture, stopping it...",
                             self.tests_run)
            self.tests_run = 0

            teardown_success = self.fixture.teardown()
            self.logger.info("Starting the fixture back up again...")
            self.fixture.setup()
            self.fixture.await_ready()

            # Raise this after calling setup in case --continueOnFailure was specified.
            if not teardown_success:
                raise errors.TestFailure("%s did not exit cleanly" % (self.fixture))


class JsCustomBehavior(CustomBehavior):
    def __init__(self, logger, fixture, js_filename, description, shell_options=None):
        CustomBehavior.__init__(self, logger, fixture, description)
        self.hook_test_case = testcases.JSTestCase(logger,
                                                   js_filename,
                                                   shell_options=shell_options,
                                                   test_kind="Hook")
        self.test_case_is_configured = False

    def before_suite(self, test_report):
        if not self.test_case_is_configured:
            # Configure the test case after the fixture has been set up.
            self.hook_test_case.configure(self.fixture)
            self.test_case_is_configured = True

    def after_test(self, test, test_report):
        description = "{0} after running '{1}'".format(self.description, test.short_name())
        try:
            # Change test_name and description to be more descriptive.
            self.hook_test_case.test_name = test.short_name() + ":" + self.logger_name
            CustomBehavior.start_dynamic_test(self.hook_test_case, test_report)
            self.hook_test_case.run_test()
            self.hook_test_case.return_code = 0
            test_report.addSuccess(self.hook_test_case)
        except self.hook_test_case.failureException as err:
            self.hook_test_case.logger.exception("{0} failed".format(description))
            test_report.addFailure(self.hook_test_case, sys.exc_info())
            raise errors.TestFailure(err.args[0])
        finally:
            test_report.stopTest(self.hook_test_case)

class BackgroundInitialSync(JsCustomBehavior):
    """
    After every test, this hook checks if a background node has finished initial sync and if so,
    validates it, tears it down, and restarts it.

    This test accepts a parameter 'n' that specifies a number of tests after which it will wait for
    replication to finish before validating and restarting the initial sync node. It also accepts
    a parameter 'use_resync' for whether to restart the initial sync node with resync or by
    shutting it down and restarting it.

    This requires the ReplicaSetFixture to be started with 'start_initial_sync_node=True'. If used
    at the same time as CleanEveryN, the 'n' value passed to this hook should be equal to the 'n'
    value for CleanEveryN.
    """

    DEFAULT_N = CleanEveryN.DEFAULT_N

    def __init__(self, logger, fixture, use_resync=False, n=DEFAULT_N):
        description = "Background Initial Sync"
        js_filename = os.path.join("jstests", "hooks", "run_initial_sync_node_validation.js")
        JsCustomBehavior.__init__(self, logger, fixture, js_filename, description)

        self.use_resync = use_resync
        self.n = n
        self.tests_run = 0
        self.random_restarts = 0


    def after_test(self, test, test_report):
        self.tests_run += 1
        sync_node = self.fixture.get_initial_sync_node();
        sync_node_conn = utils.new_mongo_client(port=sync_node.port)
        description = "{0} after running '{1}'".format(self.description, test.short_name())

        # Restarts initial sync by shutting down the node, clearing its data, and restarting it,
        # or by calling resync if use_resync is specified.
        def restart_init_sync():
            if self.use_resync:
                self.fixture.logger.info("Calling resync on initial sync node...")
                cmd = bson.SON([("resync", 1), ("wait", 0)])
                try:
                    sync_node_conn.admin.command(cmd)
                except pymongo.errors.OperationFailure as err:
                    self.fixture.logger.exception("{0} failed".format(description))
                    test_report.addFailure(self.hook_test_case, sys.exc_info())
                    raise errors.TestFailure(err.args[0])
            else:
                # Tear down and restart the initial sync node to start initial sync again.
                teardown_success = sync_node.teardown()

                self.fixture.logger.info("Starting the initial sync node back up again...")
                sync_node.setup()
                sync_node.await_ready()
                if not teardown_success:
                    raise errors.TestFailure("%s did not exit cleanly" % (sync_node))

        # If it's been 'n' tests so far, wait for the initial sync node to finish syncing.
        if self.tests_run >= self.n:
            self.tests_run = 0
            self.fixture.logger.info(
                "%d tests have been run against the fixture, waiting for initial sync"
                " node to go into SECONDARY state",
                self.tests_run)
            cmd = bson.SON([("replSetTest", 1),
                            ("waitForMemberState", 2),
                            ("timeoutMillis", 20 * 60 * 1000)])
            try:
                sync_node_conn.admin.command(cmd)
            except pymongo.errors.OperationFailure as err:
                self.fixture.logger.exception("{0} failed".format(description))
                test_report.addFailure(self.hook_test_case, sys.exc_info())
                raise errors.TestFailure(err.args[0])

        # Check if the initial sync node is in SECONDARY state. If it's been 'n' tests, then it
        # should have waited to be in SECONDARY state and the test should be marked as a failure.
        # Otherwise, we just skip the hook and will check again after the next test.
        try:
            state = sync_node_conn.admin.command("replSetGetStatus").get("myState")
            if state != 2:
                if self.tests_run == 0:
                    msg = "Initial sync node did not catch up after waiting 20 minutes"
                    self.fixture.logger.exception("{0} failed: {1}".format(description, msg))
                    test_report.addFailure(self.hook_test_case, sys.exc_info())
                    raise errors.TestFailure(msg)

                self.fixture.logger.info(
                    "Initial sync node is in state %d, not state SECONDARY (2)."
                    " Skipping BackgroundInitialSync hook for %s",
                    state,
                    test.short_name())

                # If we have not restarted initial sync since the last time we ran the data
                # validation, and if we're using resync for restarts, restart initial sync with a
                # 20% probability.
                if self.random_restarts < 1 and self.use_resync and random.random() < 0.2:
                    self.fixture.logger.info(
                        "Calling resync randomly in the middle of initial sync")
                    restart_init_sync()
                    self.random_restarts += 1
                return
        except pymongo.errors.OperationFailure:
            # replSetGetStatus can fail if the node is in STARTUP state. The node will soon go into
            # STARTUP2 state and replSetGetStatus will succeed after the next test.
            self.fixture.logger.info(
                "replSetGetStatus call failed in BackgroundInitialSync hook, skipping hook for %s",
                test.short_name())
            return

        self.random_restarts = 0

        # We're in SECONDARY state so validate the data. If there's a failure restart the fixture
        # so we don't get multiple occurrences of the same failure, and then rethrow the failure.
        try:
            JsCustomBehavior.after_test(self, test, test_report)
        except errors.TestFailure as err:
            self.fixture.logger.exception("{0} failed with {1}".format(description, err.args[0]))
            restart_init_sync()
            raise errors.TestFailure(err.args[0])

        restart_init_sync()


class IntermediateInitialSync(JsCustomBehavior):
    """
    This hook accepts a parameter 'n' that specifies a number of tests after which it will start up
    a node to initial sync, wait for replication to finish, and then validate the data. It also
    accepts a parameter 'use_resync' for whether to restart the initial sync node with resync or by
    shutting it down and restarting it.

    This requires the ReplicaSetFixture to be started with 'start_initial_sync_node=True'.
    """

    DEFAULT_N = CleanEveryN.DEFAULT_N

    def __init__(self, logger, fixture, use_resync=False, n=DEFAULT_N):
        description = "Intermediate Initial Sync"
        js_filename = os.path.join("jstests", "hooks", "run_initial_sync_node_validation.js")
        JsCustomBehavior.__init__(self, logger, fixture, js_filename, description)

        self.use_resync = use_resync
        self.n = n
        self.tests_run = 0

    def after_test(self, test, test_report):
        self.tests_run += 1
        # If we have not run 'n' tests yet, skip this hook.
        if self.tests_run < self.n:
            return
        self.tests_run = 0

        sync_node = self.fixture.get_initial_sync_node();
        sync_node_conn = utils.new_mongo_client(port=sync_node.port)
        description = "{0} after running '{1}'".format(self.description, test.short_name())

        teardown_success = True
        if self.use_resync:
            self.fixture.logger.info("Calling resync on initial sync node...")
            cmd = bson.SON([("resync", 1)])
            try:
                sync_node_conn.admin.command(cmd)
            except pymongo.errors.OperationFailure as err:
                self.fixture.logger.exception("{0} failed".format(description))
                test_report.addFailure(self.hook_test_case, sys.exc_info())
                raise errors.TestFailure(err.args[0])
        else:
            teardown_success = sync_node.teardown()

            self.fixture.logger.info("Starting the initial sync node back up again...")
            sync_node.setup()
            sync_node.await_ready()

        # Do initial sync round.
        self.fixture.logger.info("Waiting for initial sync node to go into SECONDARY state")
        cmd = bson.SON([("replSetTest", 1),
                        ("waitForMemberState", 2),
                        ("timeoutMillis", 20 * 60 * 1000)])
        try:
            sync_node_conn.admin.command(cmd)
        except pymongo.errors.OperationFailure as err:
            self.fixture.logger.exception("{0} failed".format(description))
            test_report.addFailure(self.hook_test_case, sys.exc_info())
            raise errors.TestFailure(err.args[0])

        # Run data validation and dbhash checking.
        JsCustomBehavior.after_test(self, test, test_report)

        if not teardown_success:
            raise errors.TestFailure("%s did not exit cleanly" % (sync_node))

class ValidateCollections(JsCustomBehavior):
    """
    Runs full validation on all collections in all databases on every stand-alone
    node, primary replica-set node, or primary shard node.
    """
    def __init__(self, logger, fixture, shell_options=None):
        description = "Full collection validation"
        js_filename = os.path.join("jstests", "hooks", "run_validate_collections.js")
        JsCustomBehavior.__init__(self,
                                  logger,
                                  fixture,
                                  js_filename,
                                  description,
                                  shell_options=shell_options)


class CheckReplDBHash(JsCustomBehavior):
    """
    Checks that the dbhashes of all non-local databases and non-replicated system collections
    match on the primary and secondaries.
    """
    def __init__(self, logger, fixture, shell_options=None):
        description = "Check dbhashes of all replica set or master/slave members"
        js_filename = os.path.join("jstests", "hooks", "run_check_repl_dbhash.js")
        JsCustomBehavior.__init__(self,
                                  logger,
                                  fixture,
                                  js_filename,
                                  description,
                                  shell_options=shell_options)


class CheckReplOplogs(JsCustomBehavior):
    """
    Checks that local.oplog.rs matches on the primary and secondaries.
    """
    def __init__(self, logger, fixture, shell_options=None):
        description = "Check oplogs of all replica set members"
        js_filename = os.path.join("jstests", "hooks", "run_check_repl_oplogs.js")
        JsCustomBehavior.__init__(self,
                                  logger,
                                  fixture,
                                  js_filename,
                                  description,
                                  shell_options=shell_options)


_CUSTOM_BEHAVIORS = {
    "CleanEveryN": CleanEveryN,
    "CheckReplDBHash": CheckReplDBHash,
    "CheckReplOplogs": CheckReplOplogs,
    "ValidateCollections": ValidateCollections,
    "IntermediateInitialSync": IntermediateInitialSync,
    "BackgroundInitialSync": BackgroundInitialSync,
}
