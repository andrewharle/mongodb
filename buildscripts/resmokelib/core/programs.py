"""Utility functions to create MongoDB processes.

Handles all the nitty-gritty parameter conversion.
"""

from __future__ import absolute_import

import json
import os
import os.path
import stat

from . import process as _process
from .. import config
from .. import utils

# The below parameters define the default 'logComponentVerbosity' object passed to mongod processes
# started either directly via resmoke or those that will get started by the mongo shell. We allow
# this default to be different for tests run locally and tests run in Evergreen. This allows us, for
# example, to keep log verbosity high in Evergreen test runs without polluting the logs for
# developers running local tests.

# The default verbosity setting for any tests that are not started with an Evergreen task id. This
# will apply to any tests run locally.
DEFAULT_MONGOD_LOG_COMPONENT_VERBOSITY = {"replication": {"rollback": 2}}

# The default verbosity setting for any tests running in Evergreen i.e. started with an Evergreen
# task id.
DEFAULT_EVERGREEN_MONGOD_LOG_COMPONENT_VERBOSITY = {"replication": {"heartbeats": 2, "rollback": 2}}


def default_mongod_log_component_verbosity():
    """Return the default 'logComponentVerbosity' value to use for mongod processes."""
    if config.EVERGREEN_TASK_ID:
        return DEFAULT_EVERGREEN_MONGOD_LOG_COMPONENT_VERBOSITY
    return DEFAULT_MONGOD_LOG_COMPONENT_VERBOSITY


def mongod_program(  # pylint: disable=too-many-branches
        logger, executable=None, process_kwargs=None, **kwargs):
    """Return a Process instance that starts mongod arguments constructed from 'kwargs'."""

    executable = utils.default_if_none(executable, config.DEFAULT_MONGOD_EXECUTABLE)
    args = [executable]

    # Apply the --setParameter command line argument. Command line options to resmoke.py override
    # the YAML configuration.
    suite_set_parameters = kwargs.pop("set_parameters", {})

    if config.MONGOD_SET_PARAMETERS is not None:
        suite_set_parameters.update(utils.load_yaml(config.MONGOD_SET_PARAMETERS))

    # Set default log verbosity levels if none were specified.
    if "logComponentVerbosity" not in suite_set_parameters:
        suite_set_parameters["logComponentVerbosity"] = default_mongod_log_component_verbosity()

    # orphanCleanupDelaySecs controls an artificial delay before cleaning up an orphaned chunk
    # that has migrated off of a shard, meant to allow most dependent queries on secondaries to
    # complete first. It defaults to 900, or 15 minutes, which is prohibitively long for tests.
    # Setting it in the .yml file overrides this.
    if "shardsvr" in kwargs and "orphanCleanupDelaySecs" not in suite_set_parameters:
        suite_set_parameters["orphanCleanupDelaySecs"] = 1

    # The LogicalSessionCache does automatic background refreshes in the server. This is
    # race-y for tests, since tests trigger their own immediate refreshes instead. Turn off
    # background refreshing for tests. Set in the .yml file to override this.
    if "disableLogicalSessionCacheRefresh" not in suite_set_parameters:
        suite_set_parameters["disableLogicalSessionCacheRefresh"] = True

    # There's a periodic background thread that checks for and aborts expired transactions.
    # "transactionLifetimeLimitSeconds" specifies for how long a transaction can run before expiring
    # and being aborted by the background thread. It defaults to 60 seconds, which is too short to
    # be reliable for our tests. Setting it to 24 hours, so that it is longer than the Evergreen
    # execution timeout.
    if "transactionLifetimeLimitSeconds" not in suite_set_parameters:
        suite_set_parameters["transactionLifetimeLimitSeconds"] = 24 * 60 * 60

    # The periodic no-op writer writes an oplog entry of type='n' once every 10 seconds. This has
    # the potential to mask issues such as SERVER-31609 because it allows the operationTime of
    # cluster to advance even if the client is blocked for other reasons. We should disable the
    # periodic no-op writer. Set in the .yml file to override this.
    if "replSet" in kwargs and "writePeriodicNoops" not in suite_set_parameters:
        suite_set_parameters["writePeriodicNoops"] = False

    # By default the primary waits up to 10 sec to complete a stepdown and to hand off its duties to
    # a secondary before shutting down in response to SIGTERM. Make it shut down more abruptly.
    if "replSet" in kwargs and "waitForStepDownOnNonCommandShutdown" not in suite_set_parameters:
        suite_set_parameters["waitForStepDownOnNonCommandShutdown"] = False

    _apply_set_parameters(args, suite_set_parameters)

    shortcut_opts = {
        "enableMajorityReadConcern": config.MAJORITY_READ_CONCERN,
        "nojournal": config.NO_JOURNAL,
        "nopreallocj": config.NO_PREALLOC_JOURNAL,
        "serviceExecutor": config.SERVICE_EXECUTOR,
        "storageEngine": config.STORAGE_ENGINE,
        "transportLayer": config.TRANSPORT_LAYER,
        "wiredTigerCollectionConfigString": config.WT_COLL_CONFIG,
        "wiredTigerEngineConfigString": config.WT_ENGINE_CONFIG,
        "wiredTigerIndexConfigString": config.WT_INDEX_CONFIG,
    }

    if config.STORAGE_ENGINE == "rocksdb":
        shortcut_opts["rocksdbCacheSizeGB"] = config.STORAGE_ENGINE_CACHE_SIZE
    elif config.STORAGE_ENGINE == "wiredTiger" or config.STORAGE_ENGINE is None:
        shortcut_opts["wiredTigerCacheSizeGB"] = config.STORAGE_ENGINE_CACHE_SIZE

    # These options are just flags, so they should not take a value.
    opts_without_vals = ("nojournal", "nopreallocj")

    # Have the --nojournal command line argument to resmoke.py unset the journal option.
    if shortcut_opts["nojournal"] and "journal" in kwargs:
        del kwargs["journal"]

    # Ensure that config servers run with journaling enabled.
    if "configsvr" in kwargs:
        shortcut_opts["nojournal"] = False
        kwargs["journal"] = ""

    # Command line options override the YAML configuration.
    for opt_name in shortcut_opts:
        opt_value = shortcut_opts[opt_name]
        if opt_name in opts_without_vals:
            # Options that are specified as --flag on the command line are represented by a boolean
            # value where True indicates that the flag should be included in 'kwargs'.
            if opt_value:
                kwargs[opt_name] = ""
        else:
            # Options that are specified as --key=value on the command line are represented by a
            # value where None indicates that the key-value pair shouldn't be included in 'kwargs'.
            if opt_value is not None:
                kwargs[opt_name] = opt_value

    # Override the storage engine specified on the command line with "wiredTiger" if running a
    # config server replica set.
    if "replSet" in kwargs and "configsvr" in kwargs:
        kwargs["storageEngine"] = "wiredTiger"

    # Apply the rest of the command line arguments.
    _apply_kwargs(args, kwargs)

    _set_keyfile_permissions(kwargs)

    process_kwargs = utils.default_if_none(process_kwargs, {})
    return _process.Process(logger, args, **process_kwargs)


def mongos_program(logger, executable=None, process_kwargs=None, **kwargs):
    """Return a Process instance that starts a mongos with arguments constructed from 'kwargs'."""

    executable = utils.default_if_none(executable, config.DEFAULT_MONGOS_EXECUTABLE)
    args = [executable]

    # Apply the --setParameter command line argument. Command line options to resmoke.py override
    # the YAML configuration.
    suite_set_parameters = kwargs.pop("set_parameters", {})

    if config.MONGOS_SET_PARAMETERS is not None:
        suite_set_parameters.update(utils.load_yaml(config.MONGOS_SET_PARAMETERS))

    _apply_set_parameters(args, suite_set_parameters)

    # Apply the rest of the command line arguments.
    _apply_kwargs(args, kwargs)

    _set_keyfile_permissions(kwargs)

    process_kwargs = utils.default_if_none(process_kwargs, {})
    return _process.Process(logger, args, **process_kwargs)


def mongo_shell_program(  # pylint: disable=too-many-branches,too-many-locals,too-many-statements
        logger, executable=None, connection_string=None, filename=None, process_kwargs=None,
        **kwargs):
    """Return a Process instance that starts a mongo shell.

    The shell is started with the given connection string and arguments constructed from 'kwargs'.
    """
    connection_string = utils.default_if_none(config.SHELL_CONN_STRING, connection_string)

    executable = utils.default_if_none(executable, config.DEFAULT_MONGO_EXECUTABLE)
    args = [executable]

    eval_sb = []  # String builder.
    global_vars = kwargs.pop("global_vars", {}).copy()

    shortcut_opts = {
        "enableMajorityReadConcern": (config.MAJORITY_READ_CONCERN, True),
        "noJournal": (config.NO_JOURNAL, False),
        "noJournalPrealloc": (config.NO_PREALLOC_JOURNAL, False),
        "serviceExecutor": (config.SERVICE_EXECUTOR, ""),
        "storageEngine": (config.STORAGE_ENGINE, ""),
        "storageEngineCacheSizeGB": (config.STORAGE_ENGINE_CACHE_SIZE, ""),
        "testName": (os.path.splitext(os.path.basename(filename))[0], ""),
        "transportLayer": (config.TRANSPORT_LAYER, ""),
        "wiredTigerCollectionConfigString": (config.WT_COLL_CONFIG, ""),
        "wiredTigerEngineConfigString": (config.WT_ENGINE_CONFIG, ""),
        "wiredTigerIndexConfigString": (config.WT_INDEX_CONFIG, ""),
    }

    test_data = global_vars.get("TestData", {}).copy()
    for opt_name in shortcut_opts:
        (opt_value, opt_default) = shortcut_opts[opt_name]
        if opt_value is not None:
            test_data[opt_name] = opt_value
        elif opt_name not in test_data:
            # Only use 'opt_default' if the property wasn't set in the YAML configuration.
            test_data[opt_name] = opt_default

    global_vars["TestData"] = test_data

    # Initialize setParameters for mongod and mongos, to be passed to the shell via TestData. Since
    # they are dictionaries, they will be converted to JavaScript objects when passed to the shell
    # by the _format_shell_vars() function.
    mongod_set_parameters = {}
    if config.MONGOD_SET_PARAMETERS is not None:
        if "setParameters" in test_data:
            raise ValueError("setParameters passed via TestData can only be set from either the"
                             " command line or the suite YAML, not both")
        mongod_set_parameters = utils.load_yaml(config.MONGOD_SET_PARAMETERS)

    # If the 'logComponentVerbosity' setParameter for mongod was not already specified, we set its
    # value to a default.
    mongod_set_parameters.setdefault("logComponentVerbosity",
                                     default_mongod_log_component_verbosity())

    test_data["setParameters"] = mongod_set_parameters

    if config.MONGOS_SET_PARAMETERS is not None:
        if "setParametersMongos" in test_data:
            raise ValueError("setParametersMongos passed via TestData can only be set from either"
                             " the command line or the suite YAML, not both")
        mongos_set_parameters = utils.load_yaml(config.MONGOS_SET_PARAMETERS)
        test_data["setParametersMongos"] = mongos_set_parameters

    # There's a periodic background thread that checks for and aborts expired transactions.
    # "transactionLifetimeLimitSeconds" specifies for how long a transaction can run before expiring
    # and being aborted by the background thread. It defaults to 60 seconds, which is too short to
    # be reliable for our tests. Setting it to 24 hours, so that it is longer than the Evergreen
    # execution timeout.
    if "transactionLifetimeLimitSeconds" not in test_data:
        test_data["transactionLifetimeLimitSeconds"] = 24 * 60 * 60

    if "eval_prepend" in kwargs:
        eval_sb.append(str(kwargs.pop("eval_prepend")))

    # If nodb is specified, pass the connection string through TestData so it can be used inside the
    # test, then delete it so it isn't given as an argument to the mongo shell.
    if "nodb" in kwargs and connection_string is not None:
        test_data["connectionString"] = connection_string
        connection_string = None

    for var_name in global_vars:
        _format_shell_vars(eval_sb, var_name, global_vars[var_name])

    if "eval" in kwargs:
        eval_sb.append(str(kwargs.pop("eval")))

    # Load this file to allow a callback to validate collections before shutting down mongod.
    eval_sb.append("load('jstests/libs/override_methods/validate_collections_on_shutdown.js');")

    # Load a callback to check UUID consistency before shutting down a ShardingTest.
    eval_sb.append(
        "load('jstests/libs/override_methods/check_uuids_consistent_across_cluster.js');")

    eval_str = "; ".join(eval_sb)
    args.append("--eval")
    args.append(eval_str)

    if config.SHELL_READ_MODE is not None:
        kwargs["readMode"] = config.SHELL_READ_MODE

    if config.SHELL_WRITE_MODE is not None:
        kwargs["writeMode"] = config.SHELL_WRITE_MODE

    if connection_string is not None:
        # The --host and --port options are ignored by the mongo shell when an explicit connection
        # string is specified. We remove these options to avoid any ambiguity with what server the
        # logged mongo shell invocation will connect to.
        if "port" in kwargs:
            kwargs.pop("port")

        if "host" in kwargs:
            kwargs.pop("host")

    # Apply the rest of the command line arguments.
    _apply_kwargs(args, kwargs)

    if connection_string is not None:
        args.append(connection_string)

    # Have the mongos shell run the specified file.
    args.append(filename)

    _set_keyfile_permissions(test_data)

    process_kwargs = utils.default_if_none(process_kwargs, {})
    return _process.Process(logger, args, **process_kwargs)


def _format_shell_vars(sb, path, value):
    """Format 'value' in a way that can be passed to --eval.

    If 'value' is a dictionary, then it is unrolled into the creation of
    a new JSON object with properties assigned for each key of the
    dictionary.
    """

    # Only need to do special handling for JSON objects.
    if not isinstance(value, dict):
        sb.append("%s = %s" % (path, json.dumps(value)))
        return

    # Avoid including curly braces and colons in output so that the command invocation can be
    # copied and run through bash.
    sb.append("%s = new Object()" % (path))
    for subkey in value:
        _format_shell_vars(sb, ".".join((path, subkey)), value[subkey])


def dbtest_program(logger, executable=None, suites=None, process_kwargs=None, **kwargs):
    """Return a Process instance that starts a dbtest with arguments constructed from 'kwargs'."""

    executable = utils.default_if_none(executable, config.DEFAULT_DBTEST_EXECUTABLE)
    args = [executable]

    if suites is not None:
        args.extend(suites)

    kwargs["enableMajorityReadConcern"] = config.MAJORITY_READ_CONCERN
    if config.STORAGE_ENGINE is not None:
        kwargs["storageEngine"] = config.STORAGE_ENGINE

    return generic_program(logger, args, process_kwargs=process_kwargs, **kwargs)


def generic_program(logger, args, process_kwargs=None, **kwargs):
    """Return a Process instance that starts an arbitrary executable.

    The executable arguments are constructed from 'kwargs'.

    The args parameter is an array of strings containing the command to execute.
    """

    if not utils.is_string_list(args):
        raise ValueError("The args parameter must be a list of command arguments")

    _apply_kwargs(args, kwargs)

    process_kwargs = utils.default_if_none(process_kwargs, {})
    return _process.Process(logger, args, **process_kwargs)


def _apply_set_parameters(args, set_parameter):
    """Convert key-value pairs from 'kwargs' into --setParameter key=value arguments.

    This result is appended to 'args'.
    """

    for param_name in set_parameter:
        param_value = set_parameter[param_name]
        # --setParameter takes boolean values as lowercase strings.
        if isinstance(param_value, bool):
            param_value = "true" if param_value else "false"
        args.append("--setParameter")
        args.append("%s=%s" % (param_name, param_value))


def _apply_kwargs(args, kwargs):
    """Convert key-value pairs from 'kwargs' into --key value arguments.

    This result is appended to 'args'.
    A --flag without a value is represented with the empty string.
    """

    for arg_name in kwargs:
        arg_value = str(kwargs[arg_name])
        if arg_value:
            args.append("--%s=%s" % (arg_name, arg_value))
        else:
            args.append("--%s" % (arg_name))


def _set_keyfile_permissions(opts):
    """Change the permissions of keyfiles in 'opts' to 600, (only user can read and write the file).

    This necessary to avoid having the mongod/mongos fail to start up
    because "permissions on the keyfiles are too open".

    We can't permanently set the keyfile permissions because git is not
    aware of them.
    """
    if "keyFile" in opts:
        os.chmod(opts["keyFile"], stat.S_IRUSR | stat.S_IWUSR)
    if "encryptionKeyFile" in opts:
        os.chmod(opts["encryptionKeyFile"], stat.S_IRUSR | stat.S_IWUSR)
