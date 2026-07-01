#!/bin/sh

cppgm_dump_dwarf() {
  objfile=$1
  include_loc=$2

  perl "$SCRIPT_DIR/cppgm-dwarf-facts.pl" "$objfile" "$include_loc"
}
