#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 mFat
# Run the gtk-frdp GTK4 spike against the Windows VM (see ../../rdw/FINDINGS.md).
#   ./run.sh                          stay connected
#   ./run.sh --cycles 10 --hold 6     connect/disconnect loop (leak check)
#   ./run.sh --idle-cpu 10            measure CPU while connected and idle
HERE=$(cd "$(dirname "$0")" && pwd)
INST=$HERE/../inst/lib
export LD_LIBRARY_PATH=$INST GI_TYPELIB_PATH=$INST/girepository-1.0
exec /usr/bin/python3 "$HERE/spike_frdp.py" "${HOST:-127.0.0.1}" "${PORT:-13389}" "${RDP_USER:-rdptest}" \
    "${PASSWORD_FILE:-$HERE/../../rdptest.pw}" "$@"
