#!/bin/zsh
# Rewrites a mod's code plugins (Atmosphère exefs/subsdk<N>, e.g. exlaunch plugins) once, into <mod>/exefs/nxnative/, where the host loads them whenever the mod is switched on
# (hle_exl.c dm_exl_mod_modules). Run again after the engine is updated.
#   prepare_mod_exefs.sh <mod folder> [<title ID>]      (the mod folder holds exefs/ and romfs/)
set -e
mod=${1:?usage: prepare_mod_exefs.sh <mod folder> [<title ID>]}; tid=${2:-${mod:h:t}}
here=${0:A:h}; py=${PYTHON:-python3}
[[ -d $mod/exefs ]] || { print -u2 "[RED] $mod has no exefs folder"; exit 1; }
out=$mod/exefs/nxnative; rm -rf $out.tmp; mkdir -p $out.tmp
n=0
for f in $mod/exefs/subsdk*(N.); do
  DM_GAME=$here/../games/$tid $py $here/patch_nso.py $f $out.tmp ${f:t} || { rm -rf $out.tmp; exit 1; }
  n=$((n + 1))
done
(( n )) || { rm -rf $out.tmp; print -u2 "[RED] $mod/exefs has no subsdk plugin"; exit 1; }
rm -rf $out; mv $out.tmp $out
print "prepared $n plugin(s) in $out"
