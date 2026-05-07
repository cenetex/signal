#!/usr/bin/env bash
set -euo pipefail

root="$(git rev-parse --show-toplevel)"
cd "$root"

raw_machine="${PROFILE_MACHINE:-$(hostname)}"
machine="$(printf "%s" "$raw_machine" \
    | tr '[:upper:]' '[:lower:]' \
    | tr -c '[:alnum:]_.-' '-' \
    | sed 's/-*$//')"
stamp="${PROFILE_STAMP:-$(date +%Y-%m-%d)}"
out_dir="perf/machines/$machine"
out="$out_dir/$stamp.md"
log_lines="${PROFILE_LOG_LINES:-80}"

mkdir -p "$out_dir"

sysctl_value() {
    sysctl -n "$1" 2>/dev/null || printf "unavailable"
}

time_command() {
    local title="$1"
    shift
    local tmp_log tmp_time status
    tmp_log="$(mktemp)"
    tmp_time="$(mktemp)"
    set +e
    /usr/bin/time -p "$@" >"$tmp_log" 2>"$tmp_time"
    status=$?
    set -e

    {
        printf "\n## %s\n\n" "$title"
        printf "command:"
        printf " %q" "$@"
        printf "\n\n"
        printf "\`\`\`\n"
        local total_lines
        total_lines="$(wc -l < "$tmp_log" | tr -d ' ')"
        if [ "$total_lines" -gt "$log_lines" ]; then
            printf "[stdout trimmed to last %s of %s lines]\n" "$log_lines" "$total_lines"
            tail -n "$log_lines" "$tmp_log"
        else
            cat "$tmp_log"
        fi
        cat "$tmp_time"
        printf "\`\`\`\n"
        printf "\nstatus: %d\n" "$status"
    } >> "$out"

    rm -f "$tmp_log" "$tmp_time"
    return "$status"
}

{
    printf "# Performance Profile: %s\n\n" "$raw_machine"
    printf -- "- date: %s\n" "$stamp"
    printf -- "- git: %s\n" "$(git rev-parse --short HEAD)$(git diff --quiet || printf -- "-dirty")"
    printf -- "- machine: %s\n" "$raw_machine"
    printf -- "- uname: %s\n" "$(uname -a)"
    printf -- "- model: %s\n" "$(sysctl_value hw.model)"
    printf -- "- cpu: %s\n" "$(sysctl_value machdep.cpu.brand_string)"
    printf -- "- cores: %s\n" "$(sysctl_value hw.ncpu)"
    printf "\n## Scope\n\n"
    printf "Machine-scoped build and test timing for tracking local performance drift. "
    printf "Run with \`PROFILE_MACHINE=<name> PROFILE_STAMP=YYYY-MM-DD make profile-machine\` "
    printf "to regenerate a comparable report on another machine.\n"
} > "$out"

time_command "Native Build" make build
time_command "Fast Test Suite" make test

printf "\nWrote %s\n" "$out"
