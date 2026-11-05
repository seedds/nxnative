#!/bin/zsh
# The transferable kit keeps this launcher beside run_mac_lan.py and dmhost.
setopt PIPE_FAIL
lan_kit=${0:A:h}
lan_python=''
for lan_candidate in /opt/homebrew/bin/python3 /usr/local/bin/python3 /usr/bin/python3; do
    if [[ -x "$lan_candidate" ]] && "$lan_candidate" -c 'import sys; sys.exit(sys.version_info < (3, 9))'; then
        lan_python=$lan_candidate
        break
    fi
done
if [[ -z "$lan_python" ]]; then
    print -u2 'Python 3.9 or later is required. Use the Python installed for your nxnative build.'
    exit 1
fi
print "Enter the other player's IP, select your completed build folder, then choose your in-game username."
print 'Select the completed title-ID_build folder from your existing nxnative build.'
lan_logs="$HOME/Library/Logs/nxnative/lan"
mkdir -p "$lan_logs"
lan_log="$lan_logs/lan-$(date '+%Y%m%d-%H%M%S')-$$.log"
print "Live output is also saved to $lan_log"
lan_peer_args=(--choose-peer)
# Preserve explicit --peer overrides for command-line diagnostics.
for lan_arg in "$@"; do
    if [[ "$lan_arg" == --peer || "$lan_arg" == --peer=* || "$lan_arg" == --choose-peer ]]; then
        lan_peer_args=()
        break
    fi
done
lan_name_args=(--choose-username)
for lan_arg in "$@"; do
    if [[ "$lan_arg" == --username || "$lan_arg" == --username=* || "$lan_arg" == --choose-username ]]; then
        lan_name_args=()
        break
    fi
done
"$lan_python" -u "$lan_kit/run_mac_lan.py" "${lan_peer_args[@]}" "${lan_name_args[@]}" --choose-build "$@" 2>&1 | tee "$lan_log"
lan_result=$?
if (( lan_result != 0 )); then
    print "LAN launch stopped with exit code $lan_result. See the output above."
fi
if [[ -t 0 ]]; then
    read -r '?Press Return to close this window.'
fi
exit $lan_result
