#!/bin/bash
#
# Full host test of the DIP network (FTP) backend.
#
# Copyright (C) 2026 d2x-cios-ftp contributors.
# License: GNU GPL v3 or later, see LICENSE.
#
# What it does:
#   1. builds net-test (host_shim.c + source/dip-plugin/net.c);
#   2. creates, in a temporary folder:
#        - small.iso: 64 MB of random data;
#        - big.iso:   a ~5 GB sparse file with a marker every 32 KB, to
#                     check offsets above 4 GB (REST with 64 bit values);
#   3. starts a local FTP server with pyftpdlib (user "wii", password
#      "wii", read only) on 127.0.0.1:2121;
#   4. runs the reads with and without fault injection, then a read of a
#      file that doesn't exist (it must fail cleanly);
#   5. stops the server and deletes the temporary folder.
#
# Requirements: gcc, make, python3 and pyftpdlib (pip install pyftpdlib).
# The sparse file needs a filesystem with sparse file support (ext4,
# btrfs, tmpfs...); it uses almost no real disk space.

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
PORT=2121
TMP="$(mktemp -d)"

cleanup() {
	[ -n "$FTPD_PID" ] && kill "$FTPD_PID" 2>/dev/null || true
	rm -rf "$TMP"
}
trap cleanup EXIT

make -C "$HERE" -s

echo "Creating test images in $TMP..."
mkdir -p "$TMP/root"
head -c 64M /dev/urandom > "$TMP/root/small.iso"
python3 - "$TMP/root/big.iso" <<'EOF'
import struct, sys
size = 5 * 1024**3 + 12345 * 4
with open(sys.argv[1], "wb") as f:
    f.truncate(size)
    # 16 byte marker every 32 KB: offset and offset ^ constant
    for off in range(0, size - 16, 32768):
        f.seek(off)
        f.write(struct.pack(">QQ", off, off ^ 0xdeadbeefcafebabe))
EOF

echo "Starting pyftpdlib on 127.0.0.1:$PORT..."
python3 - "$TMP/root" "$PORT" > "$TMP/ftpd.log" 2>&1 <<'EOF' &
import sys
from pyftpdlib.authorizers import DummyAuthorizer
from pyftpdlib.handlers import FTPHandler
from pyftpdlib.servers import FTPServer
auth = DummyAuthorizer()
auth.add_user("wii", "wii", sys.argv[1], perm="elr")
FTPHandler.authorizer = auth
FTPServer(("127.0.0.1", int(sys.argv[2])), FTPHandler).serve_forever()
EOF
FTPD_PID=$!
sleep 1

T="$HERE/net-test"

echo "== sequential/random reads, 64 MB image"
"$T" $PORT /small.iso "$TMP/root/small.iso" 400 wii wii

echo "== reads above 4 GB, 5 GB image"
"$T" $PORT /big.iso "$TMP/root/big.iso" 300 wii wii

echo "== fault injection (1 recv out of 150 fails)"
FAULT_RATE=150 "$T" $PORT /big.iso "$TMP/root/big.iso" 300 wii wii

echo "== missing file (every read must fail, without retry storms)"
if "$T" $PORT /missing.iso "$TMP/root/small.iso" 2 wii wii; then
	echo "FAILED: reads of a missing file succeeded"
	exit 1
fi

echo
echo "All tests passed."
