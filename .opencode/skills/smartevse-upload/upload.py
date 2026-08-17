#!/usr/bin/env python3
"""OTA upload a SmartEVSE firmware.bin to the controller over HTTP.

The SmartEVSE webserver exposes a custom chunked uploader at /update
(see network_common.cpp).  It is NOT a multipart form: each POST carries a
raw binary chunk in the body and must set the query parameters
   offset = byte offset of this chunk in the file
   file   = the exact logical filename ("firmware.bin", ...)
   size   = total size of the file in bytes
Only the names firmware.bin, firmware.debug.bin, firmware.signed.bin and
firmware.debug.signed.bin are accepted (plus rfid.txt).  When the last chunk
arrives the device finishes the OTA and reboots.

Usage:
  python3 upload.py [--host 10.0.0.91] [--file firmware.bin] [--chunk 8192]
Defaults: host=10.0.0.91, file=SmartEVSE-3/.pio/build/release/firmware.bin

Only the Python standard library is used.
"""
import argparse
import os
import socket
import sys
import time
import urllib.error
import urllib.request

MAX_SIZE = 0x1B0000          # from partitions_custom.csv, see network_common.cpp
ALLOWED = {
    "firmware.bin",
    "firmware.debug.bin",
    "firmware.signed.bin",
    "firmware.debug.signed.bin",
    "rfid.txt",
}
DEFAULT_BIN = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "..", "..", "SmartEVSE-3", ".pio", "build", "release", "firmware.bin",
)


def upload(host, binpath, logical_name, chunk_size):
    total = os.path.getsize(binpath)
    if total > MAX_SIZE:
        sys.exit(
            "firmware is %d bytes, exceeds the %d-byte update limit" % (total, MAX_SIZE)
        )

    print("Uploading %s (%s, %d bytes) to %s" % (os.path.basename(binpath), logical_name, total, host))
    base = "http://%s/update" % host

    with open(binpath, "rb") as fh:
        offset = 0
        while offset < total:
            chunk = fh.read(chunk_size)
            final = offset + len(chunk) >= total
            url = "%s?offset=%d&file=%s&size=%d" % (base, offset, logical_name, total)
            req = urllib.request.Request(url, data=chunk, method="POST")
            try:
                with urllib.request.urlopen(req, timeout=30) as resp:
                    body = resp.read().decode(errors="replace")
                    if resp.status != 200:
                        sys.exit("HTTP %d: %s" % (resp.status, body))
            except (urllib.error.HTTPError, urllib.error.URLError, TimeoutError, socket.timeout) as e:
                if final:
                    # On the last chunk the controller finishes the OTA and
                    # reboots, which drops the connection before a reply.
                    print("\nFinal chunk sent; connection closed (device rebooting) - this is expected.")
                    break
                if isinstance(e, urllib.error.HTTPError):
                    sys.exit("HTTP %d: %s" % (e.code, e.read().decode(errors="replace")))
                sys.exit("cannot reach %s: %s (is the controller on? try http://%s/update in a browser)" % (host, getattr(e, "reason", e), host))

            offset += len(chunk)
            pct = 100.0 * offset / total
            sys.stdout.write("\r%d / %d bytes (%.1f%%)" % (offset, total, pct))
            sys.stdout.flush()

            if final:
                # Give the controller a moment to finalise the update.
                time.sleep(2)

    print("\nUpload complete. The controller verifies and reboots itself.")


def main():
    ap = argparse.ArgumentParser(description="OTA-upload firmware to a SmartEVSE controller")
    ap.add_argument("--host", default="10.0.0.91", help="controller IP/hostname (default 10.0.0.91)")
    ap.add_argument("--file", default=DEFAULT_BIN, help="path to the .bin to upload")
    ap.add_argument("--chunk", type=int, default=8192, help="chunk size in bytes (default 8192)")
    args = ap.parse_args()

    if not os.path.isfile(args.file):
        sys.exit("file not found: %s (build it first: cd SmartEVSE-3 && pio run)" % args.file)

    name = os.path.basename(args.file)
    if name in ALLOWED:
        logical = name
    elif name.endswith(".bin"):
        logical = "firmware.bin"
        print("note: local file %r will be uploaded as 'firmware.bin' (the /update endpoint only accepts that name)" % name)
    else:
        sys.exit("only these logical names are accepted by /update: %s" % ", ".join(sorted(ALLOWED)))

    upload(args.host, args.file, logical, args.chunk)


if __name__ == "__main__":
    main()