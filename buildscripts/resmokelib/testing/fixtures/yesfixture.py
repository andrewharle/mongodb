"""Fixture for generating lots of log messages."""

from __future__ import absolute_import

import signal

from . import interface
from ...core import programs


class YesFixture(interface.Fixture):  # pylint: disable=abstract-method
    """Fixture which spawns several 'yes' executables to generate lots of log messages."""

    def __init__(self, logger, job_num, num_instances=1, message_length=100):
        """Initialize YesFixture."""
        interface.Fixture.__init__(self, logger, job_num)

        self.__processes = [None] * num_instances
        self.__message = "y" * message_length

    def setup(self):
        """Start the yes processes."""
        for (i, process) in enumerate(self.__processes):
            process = self._make_process(i)

            self.logger.info("Starting yes process...\n%s", process.as_command())
            process.start()
            self.logger.info("yes process started with pid %d.", process.pid)

            self.__processes[i] = process

    def _make_process(self, index):
        logger = self.logger.new_fixture_node_logger("yes{:d}".format(index))
        return programs.generic_program(logger, ["yes", self.__message])

    def _do_teardown(self):
        running_at_start = self.is_running()
        success = True  # Still a success even if nothing is running.

        if not running_at_start:
            self.logger.info(
                "yes processes were expected to be running in _do_teardown(), but weren't.")
        else:
            self.logger.info("Stopping all yes processes...")

        for process in reversed(self.__processes):
            if process is not None:
                if running_at_start:
                    self.logger.info("Stopping yes process with pid %d...", process.pid)
                    process.stop()

                exit_code = process.wait()
                success = (exit_code == -signal.SIGTERM) and success

                if running_at_start:
                    self.logger.info(
                        "Successfully terminated the yes process with pid %d, exited with code"
                        " %d.", process.pid, exit_code)

        if running_at_start:
            self.logger.info("Successfully stopped all yes processes.")

        return success

    def is_running(self):
        """Return true if the yes processes are running."""
        return all(process is not None and process.poll() is None for process in self.__processes)
