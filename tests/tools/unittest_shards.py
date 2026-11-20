# SPDX-License-Identifier: GPL-3.0-or-later
"""Runs one unittest class as several ctest entries, so a slow class fits each entry's time limit.

DSD_NEO_TEST_SHARD=<i>/<n> keeps the tests of shard i of n. A test module hands load_tests_for() its class and a
table of n lists: each list names the tests one shard runs, chosen from measured times so the shards take about as
long. A test the table does not name goes to a shard by its place among the unnamed tests in name order (shard 1,
2, ... n, 1, ...), so a new test always runs in exactly one shard. Without the variable every test of the module
runs, as before.
"""

import os
import unittest

ENV = "DSD_NEO_TEST_SHARD"


def shard_owners(names, table):
    """Test name -> its shard (1 to len(table)) for the tests of a class. A name in two lists is an error."""
    owners = {}
    for shard, listed in enumerate(table, 1):
        for name in listed:
            if name in owners:
                raise ValueError(f"{name} is listed in shards {owners[name]} and {shard}")
            owners[name] = shard
    owners = {name: shard for name, shard in owners.items() if name in names}
    unnamed = sorted(name for name in names if name not in owners)
    for place, name in enumerate(unnamed):
        owners[name] = place % len(table) + 1
    return owners


def requested_shard(count):
    """The shard DSD_NEO_TEST_SHARD asks for, or None; a value that is no shard of count is an error."""
    value = os.environ.get(ENV)
    if not value:
        return None
    shard, _, total = value.partition("/")
    if not (shard.isdigit() and total.isdigit() and int(total) == count and 1 <= int(shard) <= count):
        raise ValueError(f"{ENV}={value!r} names no shard of {count}")
    return int(shard)


def load_tests_for(case, table, first_shard_extra=()):
    """A load_tests hook: with DSD_NEO_TEST_SHARD set, only the tests of case in that shard, and in shard 1 also the
    test cases of first_shard_extra (fast checks of the module itself, such as that the shards cover the class)."""

    def load_tests(loader, tests, pattern):
        shard = requested_shard(len(table))
        if shard is None:
            return tests
        names = loader.getTestCaseNames(case)
        owners = shard_owners(names, table)
        suite = unittest.TestSuite(case(name) for name in names if owners[name] == shard)
        if shard == 1:
            for extra in first_shard_extra:
                suite.addTests(loader.loadTestsFromTestCase(extra))
        return suite

    return load_tests


def shard_runs(case, table, first_shard_extra=()):
    """What load_tests_for() runs in each shard in turn: a list of (test case class, test name) per shard."""
    loader = unittest.TestLoader()
    hook = load_tests_for(case, table, first_shard_extra)
    saved = os.environ.get(ENV)
    runs = []
    try:
        for shard in range(1, len(table) + 1):
            os.environ[ENV] = f"{shard}/{len(table)}"
            runs.append([(type(test), test._testMethodName) for test in hook(loader, unittest.TestSuite(), None)])
    finally:
        if saved is None:
            os.environ.pop(ENV, None)
        else:
            os.environ[ENV] = saved
    return runs


def check_shards(test, case, table, first_shard_extra, cmake_text, entry):
    """Asserts that the shards run every test of case exactly once, that the table names only tests the class has,
    that a test it does not name still runs once, that the extra cases run in shard 1 only, and that ctest registers
    one entry per shard (entry_1 to entry_n) that sets DSD_NEO_TEST_SHARD and names no class, so the module's
    load_tests hook picks the tests."""
    names = list(unittest.TestLoader().getTestCaseNames(case))
    listed = [name for shard in table for name in shard]
    test.assertEqual(len(listed), len(set(listed)), "a test is listed in two shards")
    test.assertLessEqual(set(listed), set(names), "the table names a test the class does not have")
    runs = shard_runs(case, table, first_shard_extra)
    ran = [name for run in runs for cls, name in run if cls is case]
    test.assertEqual(sorted(ran), sorted(names))
    for extra in first_shard_extra:
        test.assertEqual([sum(cls is extra for cls, _ in run) > 0 for run in runs],
                         [True] + [False] * (len(table) - 1))
    grown = shard_owners(names + ["test_added_later", "test_another_added_later"], table)
    test.assertEqual(sorted(grown), sorted(names + ["test_added_later", "test_another_added_later"]))
    count = len(table)
    test.assertIn(f"foreach(_dsd_shard RANGE 1 {count})", cmake_text)
    block = cmake_text[cmake_text.index(f"NAME {entry}_${{_dsd_shard}}"):]
    block = block[:block.index("endforeach()")]
    test.assertIn(f"DSD_NEO_TEST_SHARD=${{_dsd_shard}}/{count}", block)
    command = block[block.index("COMMAND"):block.index(")")].split()
    test.assertTrue(command[-1].endswith(".py"), command)
    test.assertNotIn(f"NAME {entry}\n", cmake_text)
