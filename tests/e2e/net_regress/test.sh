#!/bin/sh
set -e

gcc -Wall -Wextra -O2 test_reuseport.c -o ./test_reuseport
./test_reuseport

gcc -Wall -Wextra -O2 test_recvfrom_short.c -o ./test_recvfrom_short
./test_recvfrom_short

gcc -Wall -Wextra -O2 test_passcred.c -o ./test_passcred
./test_passcred

gcc -Wall -Wextra -O2 test_recvmsg_pktinfo.c -o ./test_recvmsg_pktinfo
./test_recvmsg_pktinfo

gcc -Wall -Wextra -O2 -pthread test_recvmsg_eintr.c -o ./test_recvmsg_eintr
./test_recvmsg_eintr

gcc -Wall -Wextra -O2 test_sockaddr_len.c -o ./test_sockaddr_len
./test_sockaddr_len

gcc -Wall -Wextra -O2 test_sockopt_state.c -o ./test_sockopt_state
./test_sockopt_state

gcc -Wall -Wextra -O2 test_unix_peername.c -o ./test_unix_peername
./test_unix_peername

gcc -Wall -Wextra -O2 test_netlink_route.c -o ./test_netlink_route
./test_netlink_route
