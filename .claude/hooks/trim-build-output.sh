#!/bin/bash
# Trim build/test output for ctest, ninja, cmake --build, docker (compose) build.
# Hardened vs the first draft: (1) only a SIMPLE command is rewritten -- no ; && || | & newline,
# backtick or $( -- because this hook answers permissionDecision "allow", and the first draft's
# regex would have auto-allowed `echo x; ctest; <anything>`; (2) the log is per-session ($PPID),
# so two sessions do not clobber one file. Compound commands pass through untouched (normal permission flow).
input=$(cat)
cmd=$(echo "$input" | jq -r '.tool_input.command')
re='^[[:space:]]*(ctest|ninja|cmake --build|docker (compose )?build)( |$)'
if [[ "$cmd" =~ $re ]] && [[ ! "$cmd" =~ [\;\&\|\`$'\n'] ]] && [[ "$cmd" != *'$('* ]]; then
  log="/tmp/cc-last-$PPID.log"
  filtered="( { ${cmd}"$'\n'"} > $log 2>&1; rc=\$?; grep -E -B1 -A6 '(error:|FAILED|Failed|undefined reference|Sanitizer)' $log | head -150; echo '--- tail ---'; tail -n 15 $log; echo '(full log: $log)'; exit \$rc )"
  echo "$input" | jq --arg f "$filtered" '{hookSpecificOutput:{hookEventName:"PreToolUse",permissionDecision:"allow",updatedInput:(.tool_input+{command:$f})}}'
else
  echo "{}"
fi
