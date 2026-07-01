#!/bin/sh
set -e

if [ "$1" != "-o" ] || [ $# -lt 3 ]; then
  echo "usage: $0 -o <outfile> <input>..." >&2
  exit 1
fi

outfile=$2
shift 2

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd) || exit 1
. "$SCRIPT_DIR/cppgm-dwarf-dump-common.sh"

objfile="${outfile}.o"
cleanup() {
  rm -f "$objfile"
}
trap cleanup EXIT INT TERM

"${CPPGM_CPPGM_APP:-../dev/cppgm++}" -c -g0 -O0 -o "$objfile" "$@"
cppgm_dump_dwarf "$objfile" 1 > "$outfile"
