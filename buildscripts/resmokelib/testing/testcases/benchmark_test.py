"""The unittest.TestCase for tests using a MongoDB vendored version of Google Benchmark."""

from __future__ import absolute_import

from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib import core
from buildscripts.resmokelib import parser
from buildscripts.resmokelib import utils
from buildscripts.resmokelib.testing.testcases import interface


class BenchmarkTestCase(interface.ProcessTestCase):
    """A Benchmark test to execute."""

    REGISTERED_NAME = "benchmark_test"

    def __init__(self, logger, program_executable, program_options=None):
        """Initialize the BenchmarkTestCase with the executable to run."""

        interface.ProcessTestCase.__init__(self, logger, "Benchmark test", program_executable)
        parser.validate_benchmark_options()

        self.bm_executable = program_executable
        self.suite_bm_options = program_options
        self.bm_options = {}

    def configure(self, fixture, *args, **kwargs):
        """Configure BenchmarkTestCase."""
        interface.ProcessTestCase.configure(self, fixture, *args, **kwargs)

        # 1. Set the default benchmark options, including the out file path, which is based on the
        #    executable path. Keep the existing extension (if any) to simplify parsing.
        bm_options = {
            "benchmark_out": self.report_name(),
            "benchmark_min_time": _config.DEFAULT_BENCHMARK_MIN_TIME.total_seconds(),
            "benchmark_repetitions": _config.DEFAULT_BENCHMARK_REPETITIONS,
            # TODO: remove the following line once we bump our Google Benchmark version to one that
            # contains the fix for https://github.com/google/benchmark/issues/559 .
            "benchmark_color": False
        }

        # 2. Override Benchmark options with options set through `program_options` in the suite
        #    configuration.
        suite_bm_options = utils.default_if_none(self.suite_bm_options, {})
        bm_options.update(suite_bm_options)

        # 3. Override Benchmark options with options set through resmoke's command line.
        resmoke_bm_options = {
            "benchmark_filter": _config.BENCHMARK_FILTER, "benchmark_list_tests":
                _config.BENCHMARK_LIST_TESTS, "benchmark_min_time": _config.BENCHMARK_MIN_TIME,
            "benchmark_out_format": _config.BENCHMARK_OUT_FORMAT,
            "benchmark_repetitions": _config.BENCHMARK_REPETITIONS
        }

        for key, value in resmoke_bm_options.items():
            if value is not None:
                # 4. sanitize options before passing them to Benchmark's command line.
                if key == "benchmark_min_time":
                    value = value.total_seconds()
                bm_options[key] = value

        self.bm_options = bm_options

    def report_name(self):
        """Return report name."""
        return self.bm_executable + ".json"

    def _make_process(self):
        return core.programs.generic_program(self.logger, [self.bm_executable], **self.bm_options)
