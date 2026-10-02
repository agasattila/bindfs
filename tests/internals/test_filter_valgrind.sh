#!/bin/sh -eu
if [ ! -x ./test_filter ]; then
    cd `dirname "$0"`
fi

if [ -n "`which valgrind`" ]; then
    valgrind --error-exitcode=100 --leak-check=full ./test_filter
else
    echo "Warning: valgrind not found. Running without."
    ./test_filter
fi
