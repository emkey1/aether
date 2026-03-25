#!/bin/sh
set -e

gcc -Wall -Wextra -O2 test_reuseport.c -o ./test_reuseport
./test_reuseport

gcc -Wall -Wextra -O2 test_recvfrom_short.c -o ./test_recvfrom_short
./test_recvfrom_short

gcc -Wall -Wextra -O2 test_passcred.c -o ./test_passcred
./test_passcred
