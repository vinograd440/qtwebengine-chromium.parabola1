#!/usr/bin/env python
#
# Copyright 2015 Google Inc.
#
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# This script does a very rough simulation of BUILD file expansion,
# mostly to see the effects of glob().

# We start by adding some symbols to our namespace that BUILD.public calls.

import glob
import pprint

def noop(*args, **kwargs):
  pass

# Simulates BUILD file glob().
def BUILD_glob(include, exclude=()):
  files = set()
  for pattern in include:
    files.update(glob.glob(pattern))
  for pattern in exclude:
    files.difference_update(glob.glob(pattern))
  return list(sorted(files))

# With these namespaces, we can treat BUILD.public as if it were
# Python code.  This pulls its variable definitions (SRCS, HDRS,
# DEFINES, etc.) into local_names.
global_names = {
  'exports_files': noop,
  'glob': BUILD_glob,
}
local_names = {}
execfile('BUILD.public', global_names, local_names)

with open('tools/BUILD.public.expected', 'w') as out:
  print >>out, "This file is auto-generated by tools/BUILD_simulator.py."
  print >>out, "It expands BUILD.public to make it easy to see changes."
  for name, value in sorted(local_names.items()):
    print >>out, name, '= ',
    pprint.pprint(value, out)
