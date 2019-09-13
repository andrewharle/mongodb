#!/usr/bin/env python
"""ADB utilities to collect adb samples from a locally connected Android device."""

import distutils.spawn  # pylint: disable=no-name-in-module
import logging
import optparse
import os
import pipes
import re
import shlex
import sys
import tempfile
import threading
import time
import warnings

# pylint: disable=wrong-import-position
# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from buildscripts.util import fileops
from buildscripts.util import runcommand

# Initialize the global logger.
LOGGER = logging.getLogger(__name__)


class Adb(object):
    """Class to abstract calls to adb."""

    def __init__(self, adb_binary="adb", logger=LOGGER):
        """Initialize the Adb object."""
        self._cmd = None
        self.logger = logger
        self.adb_path = distutils.spawn.find_executable(adb_binary)
        if not self.adb_path:
            raise EnvironmentError(
                "Executable '{}' does not exist or is not in the PATH.".format(adb_binary))

        # We support specifying a path the adb binary to use; however, systrace.py only
        # knows how to find it using the PATH environment variable. It is possible that
        # 'adb_binary' is an absolute path specified by the user, so we add its parent
        # directory to the PATH manually.
        adb_dir = os.path.dirname(self.adb_path)
        os.environ["PATH"] = "{}{}{}".format(os.environ["PATH"], os.path.pathsep, adb_dir)

        # systrace.py should be in <adb_dir>/systrace/systrace.py
        self.systrace_script = os.path.join(adb_dir, "systrace", "systrace.py")
        if not os.path.isfile(self.systrace_script):
            raise EnvironmentError("Script '{}' cannot be found.".format(self.systrace_script))
        self._tempfile = None

    @staticmethod
    def adb_cmd(adb_command, output_file=None, append_file=False, output_string=False):
        """Run an adb command and return result."""
        cmd = runcommand.RunCommand("adb {}".format(adb_command), output_file, append_file)
        if output_string or not output_file:
            return cmd.execute_with_output()
        return cmd.execute_save_output()

    @staticmethod
    def shell(adb_shell_command):
        """Run an adb shell command and return output_string.

        Raise an exception if the exit status is non-zero.

        Since the adb shell command does not return an exit status. We simulate it by
        saving the exit code in the output and then stripping if off.

        See https://stackoverflow.com/questions/9379400/adb-error-codes
        """
        cmd_prefix = "set -o errexit; function _exit_ { echo __EXIT__:$?; } ; trap _exit_ EXIT ;"
        cmd = runcommand.RunCommand("adb shell {} {}".format(cmd_prefix, adb_shell_command))
        cmd_output = cmd.execute_with_output()
        if "__EXIT__" in cmd_output:
            exit_code = int(cmd_output.split()[-1].split(":")[1])
            cmd_output_stripped = re.split("__EXIT__.*\n", cmd_output)[0]
            if exit_code:
                raise RuntimeError("{}: {}".format(exit_code, cmd_output_stripped))
            return cmd_output_stripped
        return cmd_output

    def devices(self):
        """Return the available ADB devices and the uptime."""
        return self.adb_cmd("devices -l", output_string=True)

    def device_available(self):
        """Return the the uptime of the connected device."""
        # If the device is not available this will throw an exception.
        return self.adb_cmd("shell uptime", output_string=True)

    def push(self, files, remote_dir, sync=False):
        """Push a list of files over adb to remote_dir."""
        # We can specify files as a single file name or a list of files.
        if isinstance(files, list):
            files = " ".join(files)
        sync_opt = "--sync " if sync else ""
        return self.adb_cmd("push {}{} {}".format(sync_opt, files, remote_dir), output_string=True)

    def pull(self, files, local_dir):
        """Pull a list of remote files over adb to local_dir."""
        # We can specify files as a single file name or a list of files.
        if isinstance(files, list):
            files = " ".join(files)
        return self.adb_cmd("pull {} {}".format(files, local_dir), output_string=True)

    def _battery_cmd(self, option, output_file=None, append_file=False):
        self.adb_cmd("shell dumpsys batterystats {}".format(option), output_file, append_file)

    def battery(self, reset=False, output_file=None, append_file=False):
        """Collect the battery stats and save to the output_file."""
        if reset:
            self._battery_cmd("--reset")
        self._battery_cmd("--checkin", output_file, append_file)

    def memory(self, output_file=None, append_file=False):
        """Collect the memory stats and save to the output_file."""
        self.adb_cmd("shell dumpsys meminfo -c -d", output_file, append_file)

    def systrace_start(self, output_file=None):
        """Start the systrace.py script to collect CPU usage."""
        self._tempfile = tempfile.NamedTemporaryFile(delete=False).name
        self._cmd = runcommand.RunCommand(output_file=self._tempfile, propagate_signals=False)
        self._cmd.add_file(sys.executable)
        self._cmd.add_file(self.systrace_script)
        self._cmd.add("--json")
        self._cmd.add("-o")
        self._cmd.add_file(output_file)
        self._cmd.add("dalvik sched freq idle load")
        self._cmd.start_process()

    def systrace_stop(self, output_file=None):
        """Stop the systrace.py script."""
        self._cmd.send_to_process("bye")
        with open(self._tempfile) as fh:
            buff = fh.read()
        os.remove(self._tempfile)
        self.logger.debug("systrace_stop: %s", buff)
        if "Wrote trace" not in buff:
            self.logger.error("CPU file not saved: %s", buff)
            if os.path.isfile(output_file):
                os.remove(output_file)


class AdbControl(object):  # pylint: disable=too-many-instance-attributes
    """Class to controls calls to adb."""

    _JOIN_TIMEOUT = 24 * 60 * 60  # 24 hours (a long time to have the monitor run for)

    def __init__(  # pylint: disable=too-many-arguments
            self, adb, logger=LOGGER, battery_file=None, memory_file=None, cpu_file=None,
            append_file=False, num_samples=0, collection_time_secs=0, sample_interval_ms=0):
        """Initialize AdbControl object."""

        self.adb = adb

        self.logger = logger

        output_files = [battery_file, memory_file, cpu_file]
        if not any(output_files):
            raise ValueError("There are no collection sample files selected.")
        self.battery_file = battery_file
        self.memory_file = memory_file
        self.cpu_file = cpu_file

        # The AdbResourceMonitor will always append results to the specified file.
        # If append_file is specified in this init, then if there's an existing file
        # we do not overwrite it.
        for output_file in output_files:
            if not append_file:
                fileops.create_empty(output_file)

        self.append_file = append_file
        # collection_time_secs overrides num_samples
        self.num_samples = num_samples if collection_time_secs == 0 else 0
        self.collection_time_secs = collection_time_secs
        self.sample_interval_ms = sample_interval_ms

        self.should_stop = threading.Event()
        self.should_stop.clear()
        self.sample_based_threads = []
        self.all_threads = []

    def start(self):
        """Start adb sample collection."""
        if self.cpu_file:
            monitor = AdbContinuousResourceMonitor(self.cpu_file, self.should_stop,
                                                   self.adb.systrace_start, self.adb.systrace_stop)
            self.all_threads.append(monitor)
            monitor.start()

        if self.battery_file:
            monitor = AdbSampleBasedResourceMonitor(self.battery_file, self.should_stop,
                                                    self.adb.battery, self.num_samples,
                                                    self.sample_interval_ms)
            self.sample_based_threads.append(monitor)
            self.all_threads.append(monitor)
            monitor.start()

        if self.memory_file:
            monitor = AdbSampleBasedResourceMonitor(self.memory_file, self.should_stop,
                                                    self.adb.memory, self.num_samples,
                                                    self.sample_interval_ms)
            self.sample_based_threads.append(monitor)
            self.all_threads.append(monitor)
            monitor.start()

    def stop(self):
        """Stop adb sample collection."""
        self.should_stop.set()
        self.wait()

    def wait(self):
        """Wait for all sample collections to complete."""
        try:
            # We either wait for the specified amount of time or for the sample-based monitors
            # to have collected the specified number of samples.
            if self.collection_time_secs > 0:
                self.should_stop.wait(self.collection_time_secs)
            else:
                for thread in self.sample_based_threads:
                    # We must specify a timeout to threading.Thread.join() to ensure that the
                    # wait is interruptible. The main thread would otherwise never be able to
                    # receive a KeyboardInterrupt.
                    thread.join(self._JOIN_TIMEOUT)
        except KeyboardInterrupt:
            # The user has interrupted the script, so we signal to all of the monitor threads
            # that they should exit as quickly as they can.
            pass
        finally:
            self.should_stop.set()
            # Wait for all of the monitor threads to exit, by specifying a timeout to
            # threading.Thread.join() in case the user tries to interrupt the script again.
            for thread in self.all_threads:
                thread.join(self._JOIN_TIMEOUT)

        self.logger.info("Collections stopped.")

        # If any of the monitor threads encountered an error, then reraise the exception in the
        # main thread.
        for thread in self.all_threads:
            if thread.exception is not None:
                raise thread.exception


class AdbResourceMonitor(threading.Thread):
    """Thread to collect information about a specific resource using adb."""

    def __init__(self, output_file, should_stop, logger=LOGGER):
        """Initialize the AdbResourceMonitor object."""
        threading.Thread.__init__(self, name="AdbResourceMonitor {}".format(output_file))
        self._output_file = output_file
        self._should_stop = should_stop
        self.logger = logger
        self.exception = None

    def run(self):
        """Collect adb samples."""
        try:
            self._do_monitoring()
        except Exception as err:  # pylint: disable=broad-except
            self.logger.error("%s: Encountered an error: %s", self._output_file, err)
            self.exception = err
            self._should_stop.set()


class AdbSampleBasedResourceMonitor(AdbResourceMonitor):
    """Subclass for ADB sample based monitor."""

    def __init__(  # pylint: disable=too-many-arguments
            self, output_file, should_stop, adb_cmd, num_samples, sample_interval_ms):
        """Initialize AdbSampleBasedResourceMonitor."""
        AdbResourceMonitor.__init__(self, output_file, should_stop)
        self.adb_cmd = adb_cmd
        self._num_samples = num_samples
        self._sample_interval_ms = sample_interval_ms

    def _do_monitoring(self):
        """Monitor function."""
        collected_samples = 0
        now = time.time()

        while not self._should_stop.is_set():
            if self._num_samples > 0 and collected_samples >= self._num_samples:
                break
            if collected_samples > 0:
                self.logger.debug("%s: Sleeping %d ms.", self._output_file,
                                  self._sample_interval_ms)
                self._should_stop.wait(self._sample_interval_ms / 1000.0)
            collected_samples += 1
            self._take_sample(collected_samples)

        total_time_ms = (time.time() - now) * 1000
        self.logger.info("%s: Stopping monitoring, %d samples collected in %d ms.",
                         self._output_file, collected_samples, total_time_ms)

    def _take_sample(self, collected_samples):
        """Collect sample."""
        self.logger.debug("%s: Collecting sample %d of %d", self._output_file, collected_samples,
                          self._num_samples)
        self.adb_cmd(output_file=self._output_file, append_file=True)


class AdbContinuousResourceMonitor(AdbResourceMonitor):
    """Subclass for ADB continuous sample based monitoring."""

    def __init__(self, output_file, should_stop, adb_start_cmd, adb_stop_cmd):
        """Initialize AdbContinuousResourceMonitor."""
        AdbResourceMonitor.__init__(self, output_file, should_stop)
        self._adb_start_cmd = adb_start_cmd
        self._adb_stop_cmd = adb_stop_cmd

    def _do_monitoring(self):
        """Monitor function."""
        self.logger.debug("%s: Starting monitoring.", self._output_file)
        now = time.time()
        self._adb_start_cmd(output_file=self._output_file)
        self._should_stop.wait()
        total_time_ms = (time.time() - now) * 1000
        self.logger.info("%s: Stopping monitoring after %d ms.", self._output_file, total_time_ms)
        self._adb_stop_cmd(output_file=self._output_file)


def main():  #pylint: disable=too-many-statements
    """Execute Main program."""

    logging.basicConfig(format="%(asctime)s %(levelname)s %(message)s", level=logging.INFO)
    logging.Formatter.converter = time.gmtime

    parser = optparse.OptionParser()

    program_options = optparse.OptionGroup(parser, "Program Options")
    battery_options = optparse.OptionGroup(parser, "Battery Options")
    memory_options = optparse.OptionGroup(parser, "Memory Options")
    systrace_options = optparse.OptionGroup(parser, "Systrace Options")

    program_options.add_option("--adbBinary", dest="adb_binary",
                               help="The path for adb. Defaults to '%default', which is in $PATH.",
                               default="adb")

    program_options.add_option(
        "--samples", dest="num_samples",
        help="Number of samples to collect, 0 indicates infinite. [Default: %default]", type=int,
        default=0)

    program_options.add_option(
        "--collectionTime", dest="collection_time_secs",
        help="Time in seconds to collect samples, if specifed overrides '--samples'.", type=int,
        default=0)

    program_options.add_option(
        "--sampleIntervalMs", dest="sample_interval_ms",
        help="Time in milliseconds between collecting a sample. [Default: %default]", type=int,
        default=500)

    log_levels = ["debug", "error", "info", "warning"]
    program_options.add_option(
        "--logLevel", dest="log_level", choices=log_levels,
        help="The log level. Accepted values are: {}. [default: '%default'].".format(log_levels),
        default="info")

    battery_options.add_option(
        "--batteryFile", dest="battery_file",
        help="The destination file for battery stats (CSV format).  [Default: %default].",
        default="battery.csv")

    battery_options.add_option("--noBattery", dest="no_battery",
                               help="Disable collection of battery samples.", action="store_true",
                               default=False)

    memory_options.add_option(
        "--memoryFile", dest="memory_file",
        help="The destination file for memory stats (CSV format). [Default: %default].",
        default="memory.csv")

    memory_options.add_option("--noMemory", dest="no_memory",
                              help="Disable collection of memory samples.", action="store_true",
                              default=False)

    systrace_options.add_option(
        "--cpuFile", dest="cpu_file",
        help="The destination file for CPU stats (JSON format). [Default: %default].",
        default="cpu.json")

    systrace_options.add_option("--noCpu", dest="no_cpu", help="Disable collection of CPU samples.",
                                action="store_true", default=False)

    parser.add_option_group(program_options)
    parser.add_option_group(battery_options)
    parser.add_option_group(memory_options)
    parser.add_option_group(systrace_options)

    options, _ = parser.parse_args()

    output_files = {}
    if options.no_battery:
        options.battery_file = None
    else:
        output_files[options.battery_file] = fileops.getmtime(options.battery_file)

    if options.no_memory:
        options.memory_file = None
    else:
        output_files[options.memory_file] = fileops.getmtime(options.memory_file)

    if options.no_cpu:
        options.cpu_file = None
    else:
        output_files[options.cpu_file] = fileops.getmtime(options.cpu_file)

    LOGGER.setLevel(options.log_level.upper())
    LOGGER.info("This program can be cleanly terminated by issuing the following command:"
                "\n\t\t'kill -INT %d'", os.getpid())

    adb = Adb(options.adb_binary)
    LOGGER.info("Detected devices by adb:\n%s%s", adb.devices(), adb.device_available())

    adb_control = AdbControl(adb=adb, battery_file=options.battery_file,
                             memory_file=options.memory_file, cpu_file=options.cpu_file,
                             num_samples=options.num_samples,
                             collection_time_secs=options.collection_time_secs,
                             sample_interval_ms=options.sample_interval_ms)

    adb_control.start()
    try:
        adb_control.wait()
    finally:
        files_saved = []
        for path in output_files:
            if fileops.getmtime(path) > output_files[path] and not fileops.is_empty(path):
                files_saved.append(path)
        LOGGER.info("Files saved: %s", files_saved)


if __name__ == "__main__":
    main()
