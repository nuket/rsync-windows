#!/bin/bash
# One pull with the sender binary given as $1 (Windows path, forward slashes),
# basis restored first.  Runs on the Linux side via `bash -s`.
export SSH_AUTH_SOCK=/run/user/$(id -u)/ssh-agent.socket
cd /tmp/rsync-w2l || exit 1
rp="$1"
cp orig.bin base/vm.bin
s=$(date +%s.%N)
rsync -rt --stats --rsync-path="$rp" -e 'ssh -c aes128-gcm@openssh.com' \
    Claude@192.168.178.86:rsync-perf/vm.bin base/ 2>&1 \
    | grep -E 'Literal|Matched|speedup' | tr -s ' ' | tr '\n' ';'
e=$(date +%s.%N)
echo " ${rp##*/}: $(echo "$e - $s" | bc) s  sha=$(sha256sum base/vm.bin | cut -c1-16)"
