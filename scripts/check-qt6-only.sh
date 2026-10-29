#!/bin/sh
# Audit literal-aware input tokens, the compiled vendor closure, and a real app link.
set -eu
if [ "$#" -ne 2 ]; then
    echo 'usage: check-qt6-only.sh <Makefile> <application-link-log>' >&2
    exit 2
fi
cd "$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)"
exec python3 - "$@" <<'PY'
import pathlib
import re
import shlex
import sys

makefile, log = map(pathlib.Path, sys.argv[1:])
failed = False

def reject(message):
    global failed
    print('FAIL: ' + message, flush=True)
    failed = True

counts = {'raw': 0, 'token-stream': 0}

legacy_ledger_path = pathlib.Path('tests/tst_regex/tst_regex.cc')
legacy_ledger_literal = 'QLatin1String( "QRegExp" )'
legacy_ledger_line = ('        if ( oldEngine != QLatin1String( "QRegExp" ) && '
                      'oldEngine != QLatin1String( "QRegularExpression" ) &&')
print('EXCEPTION legacy-pattern-ledger-vocabulary: ' + str(legacy_ledger_path) +
      ':619; reason=legacy-pattern ledger vocabulary, spec §6.5; literal=' +
      legacy_ledger_literal, flush=True)

def strip_comments(text, path):
    """Keep literals and line numbers; replace each comment with whitespace."""
    cpp = path.suffix not in {'.pro', '.pri', '.ui', '.qrc'}
    out = []
    i = 0
    while i < len(text):
        start = i
        # Raw strings may contain quotes, slashes, hashes and physical newlines.
        raw = re.match(r'(?:u8|u|U|L)?R"([^ ()\\\t\r\n]{0,16})\(', text[i:]) if cpp and text[i] in 'uULR' else None
        if raw:
            end = text.find(')' + raw[1] + '"', i + raw.end())
            i = len(text) if end < 0 else end + len(raw[1]) + 2
        elif cpp and text[i].isdigit():
            # An apostrophe in a numeric literal is a digit separator, not a quote.
            number = re.match(r"[0-9][\w.']*", text[i:])
            i += number.end()
        elif text[i] in ('"', "'"):
            quote = text[i]
            i += 1
            while i < len(text):
                if text[i] == '\\':
                    i = min(i + 2, len(text))
                elif text[i] == quote:
                    i += 1
                    break
                else:
                    i += 1
        elif (cpp and text.startswith('//', i)) or (not cpp and text[i] == '#'):
            end = text.find('\n', i)
            # C++ splices escaped newlines before recognizing line comments.
            while cpp and end >= 0 and text[i:end].rstrip('\r').endswith('\\'):
                end = text.find('\n', end + 1)
            i = len(text) if end < 0 else end
            out.append(''.join('\n' if c == '\n' else ' ' for c in text[start:i]))
            continue
        elif cpp and text.startswith('/*', i):
            end = text.find('*/', i + 2)
            i = len(text) if end < 0 else end + 2
            out.append(''.join('\n' if c == '\n' else ' ' for c in text[start:i]))
            continue
        else:
            i += 1
        out.append(text[start:i])
    return ''.join(out)

def finding(stream, message):
    counts[stream] += 1
    if stream == 'raw':
        print('RAW: ' + message, flush=True)
    else:
        reject('TOKEN-STREAM: ' + message)

def search(label, pattern, paths, flags=0):
    rx = re.compile(pattern, flags)
    for stream in counts:
        print('SEARCH ' + stream + ' ' + label + ': python-re ' + shlex.quote(pattern) + ' ' +
              ' '.join(shlex.quote(str(p)) for p in paths), flush=True)
        for path in paths:
            text = path.read_text(errors='replace')
            if stream == 'token-stream':
                text = strip_comments(text, path)
            for n, line in enumerate(text.splitlines(), 1):
                checked_line = line
                # Excuse only this fixed ledger comparison, never another literal
                # or code token, even in the same file or on the same line.
                if (stream == 'token-stream' and path == legacy_ledger_path
                        and n == 619 and line == legacy_ledger_line):
                    checked_line = line.replace(legacy_ledger_literal,
                                                ' ' * len(legacy_ledger_literal), 1)
                if rx.search(checked_line):
                    finding(stream, f'{path}:{n}: {line}')

extensions = {'.pro', '.pri', '.cc', '.hh', '.c', '.h', '.cpp', '.mm', '.ui', '.qrc'}
excluded = {'docs', 'help', 'locale', 'qtsingleapplication', 'winlibs', 'maclibs', 'opencc',
            '.git', '.superpowers', 'build'}
paths = sorted(p for p in pathlib.Path('.').rglob('*')
               if p.is_file() and p.suffix in extensions
               and not excluded.intersection(p.parts)
               and not str(p).startswith('tests/fixtures/'))
print('SCOPE: extensions=' + ','.join(sorted(extensions)) +
      '; excluded=docs,help,locale,tests/fixtures,qtsingleapplication,winlibs,maclibs,opencc'
      '; generated build and agent/git metadata are not compiled inputs', flush=True)
banned = (r'QRegExp|QTextCodec|QDesktopWidget|QX11Info|QWebView|QWebPage|QWebFrame|'
          r'QWebElement|QWebSecurityOrigin|QWebInspector|QMediaContent|MAKE_QTMULTIMEDIA_PLAYER|'
          r'IS_QT_5|Qt4x5|qt4x5[.]hh|webkit|qt6-5compat|Qt5Compat')
# Version identifiers in a conditional, including continued preprocessor guards.
branch = (r'(?s)(?:^\s*#\s*(?:if|elif)[^\n]*(?:\\\n[^\n]*)*|'
          r'^\s*[^#\n]*(?:lessThan|greaterThan|equals|contains)\([^\n]*)'
          r'(?:QT_VERSION|QT_MAJOR_VERSION|QT_MINOR_VERSION)')
search('first-party banned APIs', banned, paths)
for stream in counts:
    for path in paths:
        print('SEARCH ' + stream + ' version-branch: python-re ' + shlex.quote(branch) + ' ' + shlex.quote(str(path)), flush=True)
        text = path.read_text(errors='replace')
        if stream == 'token-stream':
            text = strip_comments(text, path)
        for match in re.finditer(branch, text, re.MULTILINE):
            finding(stream, f'{path}:{text.count(chr(10), 0, match.start()) + 1}: version branching: {match.group()}')
vendors = [pathlib.Path('qtsingleapplication/src') / name for name in
           ('qtsingleapplication.pri', 'qtsingleapplication.h', 'qtsingleapplication.cpp', 'qtlocalpeer.cpp')]
for path in vendors:
    if not path.is_file():
        reject(f'missing compiled vendor input: {path}')
        continue
    search('compiled vendor (separate input)', banned, [path])
    search('compiled vendor version identifiers', r'QT_VERSION|QT_MAJOR_VERSION|QT_MINOR_VERSION', [path])
    search('compiled vendor removed checksum overload', r'qChecksum\((?!\s*QByteArrayView\b)', [path])
if not makefile.is_file() or makefile.stat().st_size == 0:
    reject(f'missing/empty application Makefile: {makefile}')
if not log.is_file() or log.stat().st_size == 0:
    reject(f'missing/empty application link log: {log}')
else:
    # Identify the target from the generated application Makefile, not a test link
    # or arbitrary text mentioning Qt. Require a compiler-driver link, not echo.
    target = re.search(r'^TARGET\s*=\s*(\S+)', makefile.read_text(), re.MULTILINE) if makefile.is_file() else None
    if not target or pathlib.Path(target[1]).name != 'goldendict':
        reject('Makefile is not the goldendict application Makefile')
    commands = []
    for line in log.read_text(errors='replace').replace('\\\n', ' ').splitlines():
        try:
            argv = shlex.split(line)
        except ValueError:
            continue
        if not argv:
            continue
        compiler = pathlib.Path(argv[0]).name
        if compiler in ('ccache', 'sccache') and len(argv) > 1:
            argv = argv[1:]
            compiler = pathlib.Path(argv[0]).name
        if not re.fullmatch(r'(?:[\w.+-]*-)?(?:g\+\+|clang\+\+|c\+\+)(?:-[0-9.]+)?', compiler):
            continue
        if '-c' in argv or '-o' not in argv:
            continue
        i = argv.index('-o')
        if i + 1 < len(argv) and pathlib.Path(argv[i + 1]).name == 'goldendict':
            commands.append(line)
    print('SEARCH actual-link: compiler driver, -o goldendict, not -c in ' + shlex.quote(str(log)), flush=True)
    if not commands:
        reject('missing actual application linker command in verbose log')
    for token in ('Qt5', 'Qt6Compat', 'Qt6Core5Compat', 'QtMultimedia', 'Qt6Multimedia', 'QtWebKit', 'X11Extras'):
        print('SEARCH actual-link: grep -nF ' + shlex.quote(token), flush=True)
        for command in commands:
            if token in command:
                reject('banned application link token ' + token + ': ' + command)
    for command in commands:
        if 'Qt6Core' not in command:
            reject('application linker command has no Qt6Core linkage: ' + command)
print(f'AUDIT: raw={counts["raw"]} token-stream={counts["token-stream"]} '
      f'scope={len(paths) + sum(p.is_file() for p in vendors)} files '
      f'({len(paths)} first-party + {sum(p.is_file() for p in vendors)} vendor)', flush=True)
print('Qt6-only audit: ' + ('FAIL' if failed else 'PASS'), flush=True)
sys.exit(1 if failed else 0)
PY
