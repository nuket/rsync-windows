#!/bin/bash
# Pull rsync-perf/vm.bin from the Windows box with each sender binary in turn,
# restoring the basis file between runs.  Runs on the Linux side via `bash -s`.
export SSH_AUTH_SOCK=/run/user/$(id -u)/ssh-agent.socket
cd /tmp/rsync-w2l || exit 1
NEW="C:/Users/Claude/AppData/Local/Temp/claude/C--Users-Claude-devsrc-rsync-windows/66c00488-88a8-48c3-be31-6abc940b8590/scratchpad/b-harden/rsync.exe"
for rp in rsync "$NEW"; do
    cp orig.bin base/vm.bin
    s=$(date +%s.%N)
    rsync -rt --stats --rsync-path="$rp" -e 'ssh -c aes128-gcm@openssh.com' \
        Claude@192.168.178.86:rsync-perf/vm.bin base/ 2>&1 \
        | grep -E 'Literal|Matched|speedup' | tr -s ' ' | tr '\n' ';'
    e=$(date +%s.%N)
    echo " ${rp##*/}: $(echo "$e - $s" | bc) s  sha=$(sha256sum base/vm.bin | cut -c1-16)"
done
