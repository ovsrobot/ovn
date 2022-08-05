#!/usr/bin/env python3
import enum
import itertools
import os
import sys


if len(sys.argv) < 3:
    print(
        "usage: {} skipdescriptionlist testsuite\n"
        "\n"
        "This program reads two files, a skiplist containing the \n"
        "description of tests to skip separated by newline, and a \n"
        "generated testsuite script.\n"
        "\n"
        "From this it produces string with range of tests to execute \n"
        "which can be provided to the testsuite script.\n".format(sys.argv[0]),
        file=sys.stderr,
    )
    sys.exit(os.EX_USAGE)


SKIP_TEST_STRINGS = []
with open(sys.argv[1]) as fin:
    SKIP_TEST_STRINGS = [line.rstrip() for line in fin.readlines()]


@enum.unique
class State(enum.Enum):
    INIT = enum.auto()
    AT_HELP_ALL = enum.auto()


SKIP_TESTS = set()
TESTS = set()
with open(sys.argv[2]) as fin:
    state = State.INIT
    last_test = 0
    for line in fin.readlines():
        if state == State.INIT:
            if not line.startswith('at_help_all="'):
                continue
            else:
                state = State.AT_HELP_ALL
                data = line.split('"')[1].rstrip().split(";")
        elif state == State.AT_HELP_ALL:
            if line.startswith('"'):
                break
            data = line.rstrip().split(";")
        test_nr = int(data[0])
        if last_test < test_nr:
            last_test = test_nr
        for skip_string in SKIP_TEST_STRINGS:
            if skip_string in data[2]:
                SKIP_TESTS.add(test_nr)
            else:
                TESTS.add(test_nr)


def ranges(testlist):
    for a, b in itertools.groupby(
        enumerate(list(testlist)), lambda pair: pair[1] - pair[0]
    ):
        b = list(b)
        yield b[0][1], b[-1][1]


testranges = [
    "{}-{}".format(testrange[0], testrange[1])
    for testrange in ranges(TESTS - SKIP_TESTS)
]
print(" ".join(testranges))
