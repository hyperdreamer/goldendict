#!/bin/sh
# One line per invocation; each argv is a quoted, byte-escaped token. Never eval it.
if [ "${GD_TEST_WRAPPER_LOG+x}" = x ]; then
    if [ -z "$GD_TEST_WRAPPER_LOG" ] ||
       { [ -e "$GD_TEST_WRAPPER_LOG" ] && [ ! -f "$GD_TEST_WRAPPER_LOG" ]; }; then
        printf '%s\n' "test-wrapper: unusable log: $GD_TEST_WRAPPER_LOG" >&2
        exit 125
    fi
    # Encode bytes rather than lines, including CR/LF, quotes, and backslashes.
    gd_line=argv
    for gd_arg do
        gd_escaped=$(printf '%s' "$gd_arg" | LC_ALL=C od -An -v -tu1 | LC_ALL=C awk '
            { for (i = 1; i <= NF; ++i) {
                n = $i
                if (n == 34) printf "\\\""
                else if (n == 92) printf "\\\\"
                else if (n == 10) printf "\\n"
                else if (n == 13) printf "\\r"
                else if (n == 9) printf "\\t"
                else if (n < 32 || n > 126) printf "\\x%02x", n
                else printf "%c", n
            } }') || exit 125
        gd_line="$gd_line \"$gd_escaped\""
    done
    if ! (printf '%s\n' "$gd_line" >> "$GD_TEST_WRAPPER_LOG"); then
        printf '%s\n' "test-wrapper: cannot append log: $GD_TEST_WRAPPER_LOG" >&2
        exit 125
    fi
fi
exec xvfb-run -a timeout 900 "$@"
