#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run portable host checks with output on stdout (including Windows PowerShell)."""
from pathlib import Path
import sys
import unittest
root=Path(__file__).resolve().parents[1]
suite=unittest.defaultTestLoader.discover(str(root/'tests'),pattern='test_protocol_runtime.py')
result=unittest.TextTestRunner(stream=sys.stdout,verbosity=2).run(suite)
sys.exit(0 if result.wasSuccessful() else 1)
