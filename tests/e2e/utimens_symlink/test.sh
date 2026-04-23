#!/bin/sh
set -e

ln -s missing-target link

touch -h -t 202401020304.05 link

test "$(readlink link)" = "missing-target"
test ! -e missing-target

echo utimens symlink nofollow ok
