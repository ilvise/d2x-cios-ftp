# Host (PC) test of the DIP network (FTP) backend.
#
#   make                 build net-test
#   ./run-tests.sh       full run against a local pyftpdlib server
#   ./net-test <port> <ftp path> <local copy of the file> <reads> [user pass]
#
# net-test reads random/sequential ranges through source/dip-plugin/net.c,
# compiled unchanged with POSIX sockets instead of the IOS ones (see
# host_shim.c), and compares them with the local file. FAULT_RATE=N makes
# 1 recv out of N fail, to test reconnection.
#
# The include path puts this folder first, so net.c gets the stub
# types.h and syscalls.h of this folder instead of the cios-lib ones.

net-test: host_shim.c ../../source/dip-plugin/net.c ../../source/dip-plugin/net.h
	gcc -O1 -Wall -Wno-unused-function -I. -o $@ host_shim.c

clean:
	rm -f net-test
