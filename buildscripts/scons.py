#!/usr/bin/env python2
"""Scons module."""

from __future__ import print_function

import os
import sys

if (sys.version_info[0], sys.version_info[1]) != (2, 7):
    print("This version of MongoDB can only be built with Python 2.7"
          " you appear to be using version: %s" % sys.version)
    sys.exit(1)

SCONS_VERSION = os.environ.get('SCONS_VERSION', "2.5.0")

MONGODB_ROOT = os.path.abspath(os.path.dirname(os.path.dirname(__file__)))
SCONS_DIR = os.path.join(MONGODB_ROOT, 'src', 'third_party', 'scons-' + SCONS_VERSION,
                         'scons-local-' + SCONS_VERSION)

if not os.path.exists(SCONS_DIR):
    print("Could not find SCons in '%s'" % (SCONS_DIR))
    sys.exit(1)

sys.path = [SCONS_DIR] + sys.path

try:
    import SCons.Script
except ImportError as import_err:
    print("Could not import SCons from '%s'" % (SCONS_DIR))
    print("ImportError:", import_err)
    sys.exit(1)

SCons.Script.main()
