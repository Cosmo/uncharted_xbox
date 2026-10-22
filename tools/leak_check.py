#!/usr/bin/env python3
"""Keep game files, paths of this machine and credentials out of commits and packages.

    leak_check.py staged                     what is staged for the next commit (pre-commit hook)
    leak_check.py message FILE               a commit message (commit-msg hook)
    leak_check.py pre-push REMOTE [URL]      the commits a push sends, read from git's hook input
    leak_check.py commits REV... --not REV... every commit git rev-list lists (CI)
    leak_check.py tree                       every tracked file: an audit of what is already there
    leak_check.py package PATH...            release output: folders, .zip, .appx, .msix, nested too

The hooks in .githooks run it; enable them once per clone with git config core.hooksPath .githooks

Refused:
- game files: executables (eboot.bin, *.elf, *.prx and anything that starts like an ELF or SELF),
  dump folders (sce_sys, sce_module, u1data..u3data, CUSA*), archives (*.psarc, *.pak, *.pkg,
  *.sfo), prepared images (*.bin), save data and shader caches;
- local paths: user profiles (C:\\Users\\name, /home/name), absolute paths into a dump, this
  clone's folder, the folder around it, the home folder, the computer's name and the strings in
  .git/leak-check.local (one per line; it is never committed, so machine-specific drives, folders
  and names go there);
- credentials: key and certificate files, private keys and well-known token formats;
- in commits: build output (out/, dist/, *.exe, *.dll, *.pdb, *.zip, *.appx), binaries of an
  unknown kind and files over 5 MiB; in packages: debug symbols, logs and local settings.

A line that contains "leak-check: allow" is not checked for paths and credentials; files that may
break a path or binary rule on purpose are listed in ALLOWED. Exit status: 0 clean, 1 refused,
2 the check itself failed.
"""
import argparse
import codecs
import fnmatch
import io
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parent.parent
ALLOW_MARK = 'leak-check: allow'
MAX_FILE = 5 << 20
ZERO = '0' * 40
# Repository paths (globs) whose files may break the path, binary and size rules: one reviewed entry each.
ALLOWED = ['tests/data/*']
# The same for paths inside a package: AMD's FSR 4 model weights ship as .bin files.
PACKAGE_ALLOWED = ['*fsr4_shaders/*.bin']

GAME_PATHS = [
    (r'(^|/)eboot\.bin$|\.(elf|self|oelf|prx|sprx|sdll|sexe)$', 'game executable or module'),
    (r'(^|/)(sce_sys|sce_module|u[1-3]data|shareddata|dvdroot_ps4|dumps?)/', 'folder of a game dump'),
    (r'(^|/)((CUSA|PPSA|NPXS)\d{5}|EP\w+-CUSA\d{5})[^/]*/', 'folder of a game dump'),
    (r'(^|/)(keystone|license\.(dat|info)|npbind\.dat|nptitle\.dat|playgo-chunk\.dat)$', 'license or key file of a game'),
    (r'\.(psarc|pak|pkg|sfo|pfs|plt|at9|trp|wad|xvag|vag|gnf|dci|sbf|bsf|bnk)$', 'game archive or asset'),
    (r'\.bin$', 'binary image (a prepared game image or a dump file)'),
    (r'(^|/)(savedata|LocalState|Prepared)/|\.sav$', 'save data or prepared game files'),
    (r'(^|/)shader_?cache/|\.(vkcache|cache)$', 'shader or pipeline cache'),
]
COMMIT_PATHS = GAME_PATHS + [
    (r'^(out|dist|user|build|AppPackages|BundleArtifacts)/|(^|/)\.local/|^xbox/(build|AppPackages)/',
     'build output or local data'),
    (r'\.(exe|dll|pdb|lib|obj|o|a|so|dylib|ilk|exp|pyd|spv|dxil|cso)$', 'compiled file'),
    (r'\.(zip|7z|rar|appx|msix|appxbundle|msixbundle|appxupload|appxsym)$', 'archive or app package'),
    (r'\.(log|dmp)$|(^|/)(bbport|input)\.ini$', 'log, crash dump or local settings'),
    (r'\.(pfx|p12|pem|key|snk|ppk|jks|keystore|cer|crt|der)$', 'key or certificate file'),
    (r'(^|/)(\.env(\.[^/]*)?|\.netrc|\.git-credentials|\.npmrc|\.pypirc)$|(^|/)id_(rsa|dsa|ecdsa|ed25519)',
     'credentials file'),
    (r'\.credential\.xml$|\.local\.(json|ps1)$|^xbox/(devices|settings)\.json$', 'local credentials or settings'),
]
PACKAGE_PATHS = GAME_PATHS + [
    (r'(^|/)(out|user|\.local)/', 'prepared game files, saves or local data'),
    (r'\.(pdb|ilk|log|dmp)$|(^|/)(bbport|input)\.ini$', 'debug symbols, log or local settings'),
    (r'\.(pfx|p12|key|snk|ppk|jks|keystore)$|(^|/)(\.env|\.netrc|\.git-credentials)$'
     r'|(^|/)id_(rsa|dsa|ecdsa|ed25519)$|\.credential\.xml$', 'key or credentials file'),
]
COMMIT_PATHS = [(re.compile(pattern, re.I), reason) for pattern, reason in COMMIT_PATHS]
PACKAGE_PATHS = [(re.compile(pattern, re.I), reason) for pattern, reason in PACKAGE_PATHS]

GAME_MAGIC = [
    (b'\x7fELF', 'ELF executable (a game executable or module)'),
    (b'\x4f\x15\x3d\x1d', 'PS4 SELF executable'),
    (b'\x7fCNT', 'PS4 package'),
    (b'\x00PSF', 'PS4 param.sfo'),
    (b'PSAR', 'PlayStation archive'),
    (b'\xdc\xa2\x4d\x00', 'PS4 trophy file'),
]
COMMIT_MAGIC = GAME_MAGIC + [(b'MZ', 'Windows executable'), (b'PK\x03\x04', 'zip archive or app package')]
SAFE_BINARY = {'.png', '.jpg', '.jpeg', '.gif', '.bmp', '.ico', '.icns', '.webp', '.ttf', '.otf', '.woff', '.woff2'}
ZIP_LIKE = {'.zip', '.appx', '.msix', '.appxbundle', '.msixbundle', '.appxupload'}

# A user name in a path; placeholders (<you>, %USERNAME%, $USER) and generic accounts are not leaks.
USER = r'(?P<user>[^\\/\x00-\x1f"\'<>|*?:;,]{1,64})'
GENERIC_USERS = {'user', 'username', 'user name', 'you', 'yourname', 'your name', 'yourusername', 'me', 'name',
                 'public', 'default', 'default user', 'defaultuser0', 'all users', 'shared', 'guest',
                 'runner', 'runneradmin', 'deck', 'ubuntu', 'builder', 'nobody', 'example'}


def real_user(match):
    name = match.group('user').strip().lower()
    return bool(name) and name[0] not in '%${(<[*.~' and name not in GENERIC_USERS \
        and name.split()[0] not in GENERIC_USERS


def real_dump(match):
    """Not an example such as /path/to/CUSA03173 or D:\\here\\CUSA02343, and not where the Xbox app looks
    for the game: a USB drive's \\dumps folder or Developer Mode's D:\\DevelopmentFiles."""
    if re.match(r'(?:E:[\\/]+dumps|D:[\\/]+DevelopmentFiles)[\\/]', match.group(), re.I):
        return False
    folders = {folder.lower() for folder in re.split(r'[\\/]+', match.group())}
    return not folders & {'path', 'to', 'here', 'your', 'some', 'example', 'folder', 'путь', 'к', '...'} \
        and not re.search(r'[<>*%$]', match.group())


def real_secret(match):
    value = match.group('value').lower()
    return not any(word in value for word in
                   ('example', 'changeme', 'your', 'xxx', '***', 'dummy', 'placeholder', '$', '{', '<', '%'))


PATH_RULES = [
    (re.compile(r'(?<![A-Za-z])[A-Za-z]:[\\/]+Users[\\/]+' + USER, re.I), 'user profile path', real_user),
    (re.compile(r'(?<![\w.])/(?:mnt/|cygdrive/)?[A-Za-z]/Users/' + USER, re.I), 'user profile path', real_user),
    (re.compile(r'(?<![\w.:~])/(?:home|Users)/(?P<user>[A-Za-z0-9._-]{1,32})'), 'user profile path', real_user),
    (re.compile(r'(?<![A-Za-z])[A-Za-z]:[\\/][^\s"\'<>|]*?(?:CUSA|PPSA)\d{5}', re.I), 'path into a game dump', real_dump),
    (re.compile(r'(?<![\w.~:/])/(?:[^\s"\'<>|/]+/)+?(?:CUSA|PPSA)\d{5}'), 'path into a game dump', real_dump),
]
SECRET_RULES = [
    (re.compile(r'-----BEGIN (?:[A-Z0-9]+ )*PRIVATE KEY(?: BLOCK)?-----(?:\r?\n[\w-]+:[^\n]*)*\s*[A-Za-z0-9+/=]{40}'),
     'private key', None),
    (re.compile(r'\b(?:gh[pousr]_[A-Za-z0-9]{36,}|github_pat_\w{60,})'), 'GitHub token', None),
    (re.compile(r'\b(?:AKIA|ASIA)[0-9A-Z]{16}\b'), 'AWS access key', None),
    (re.compile(r'\bAIza[\w-]{35}'), 'Google API key', None),
    (re.compile(r'\bxox[abposr]-[\w-]{10,}'), 'Slack token', None),
    (re.compile(r'\bsk-(?:ant-|proj-)?[\w-]{32,}'), 'API secret key', None),
    (re.compile(r'\b(?:AccountKey|SharedAccessKey)=[A-Za-z0-9+/=]{30,}'), 'Azure key', None),
    (re.compile(r'discord(?:app)?\.com/api/webhooks/\d+/[\w-]{20,}', re.I), 'Discord webhook', None),
    (re.compile(r'\b(?:password|passwd|secret|client_secret|api[_-]?key|access[_-]?token|auth[_-]?token)\b'
                r'["\']?\s*[:=]\s*["\'](?P<value>[^"\'\s]{8,})["\']', re.I), 'hard-coded secret', real_secret),
]
SECRET_REASONS = {reason for _, reason, _ in SECRET_RULES}
# Third-party binaries (NVIDIA's DLSS, GnuTLS) carry their vendors' build paths and PEM strings; in
# binaries only this machine's paths, dump paths and tokens count.
NOT_IN_BINARIES = {'user profile path', 'private key'}
ASCII_RUNS = re.compile(rb'[\x20-\x7e]{6,}')
UTF16_RUNS = re.compile(rb'(?:[\x20-\x7e]\x00){6,}')


def git(*args, check=True):
    result = subprocess.run(['git', '-c', 'core.quotePath=false', *args], cwd=ROOT, capture_output=True)
    if check and result.returncode:
        raise RuntimeError(f"git {' '.join(args)}: {result.stderr.decode(errors='replace').strip()}")
    return result.stdout


def path_pattern(path):
    """A path in any spelling: C:\\x, C:/x, C:\\\\x (JSON), /c/x (MSYS2), /mnt/c/x (WSL)."""
    drive = re.match(r'([A-Za-z]):[\\/]*(.*)$', path)
    if drive:
        letter, rest = drive.groups()
        head = rf'(?:(?<![A-Za-z]){letter}:|(?<![\w.])/(?:mnt/|cygdrive/)?{letter})'
    else:
        head, rest = '', path
    parts = [re.escape(part) for part in re.split(r'[\\/]+', rest) if part]
    if not parts:
        return None
    return re.compile(head + ''.join(r'[\\/]+' + part for part in parts) + r'(?![\w.-])', re.I)


def local_rules():
    """This machine: the home folder, this clone and the folder around it, the computer's name and
    the lines of .git/leak-check.local."""
    folders = {str(Path.home()), str(ROOT)}
    if len(ROOT.parent.parts) > 1 and ROOT.parent != Path.home():
        folders.add(str(ROOT.parent))
    words = set()
    host = os.environ.get('COMPUTERNAME') or socket.gethostname()
    if len(host) >= 4:
        words.add(host)
    try:
        common = Path(git('rev-parse', '--git-common-dir').decode().strip())
        extra = (common if common.is_absolute() else ROOT / common) / 'leak-check.local'
        for line in extra.read_text(encoding='utf-8').splitlines():
            line = line.strip()
            if line and not line.startswith('#'):
                (folders if re.search(r'[\\/]|^[A-Za-z]:', line) else words).add(line)
    except (OSError, RuntimeError):
        pass
    rules = [(pattern, 'path from this machine', None) for pattern in map(path_pattern, sorted(folders)) if pattern]
    rules += [(re.compile(rf'(?<!\w){re.escape(word)}(?!\w)', re.I), 'name from this machine', None)
              for word in sorted(words)]
    return rules


class Blobs:
    """git cat-file --batch: object contents by name (:path, commit:path or an object id)."""

    def __init__(self):
        self.process = subprocess.Popen(['git', 'cat-file', '--batch'], cwd=ROOT,
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def read(self, name):
        self.process.stdin.write(name.encode() + b'\n')
        self.process.stdin.flush()
        header = self.process.stdout.readline().split()
        if len(header) != 3:
            raise RuntimeError(f'git cat-file: no object {name}')
        data = self.process.stdout.read(int(header[2]) + 1)[:-1]
        return data if header[1] == b'blob' else None


def added_lines(diff):
    """{path: (line numbers, text)} of the + lines of a git diff -U0 --no-prefix."""
    files, path, number = {}, None, 0
    for raw in diff.split(b'\n'):
        if raw.startswith(b'+++ '):
            name = raw[4:].rstrip(b'\t').decode('utf-8', 'replace')
            if name.startswith('"'):
                name = codecs.escape_decode(name[1:-1].encode())[0].decode('utf-8', 'replace')
            path = None if name == '/dev/null' else name
            if path is not None:
                files[path] = ([], [])
        elif raw.startswith(b'@@'):
            number = int(re.match(rb'@@ -\S+ \+(\d+)', raw).group(1))
        elif raw.startswith(b'+') and path is not None:
            files[path][0].append(number)
            files[path][1].append(raw[1:].decode('utf-8', 'replace'))
            number += 1
    return {path: (numbers, '\n'.join(lines)) for path, (numbers, lines) in files.items()}


def merge_lines(patches):
    """added_lines of a merge, one patch against each parent: only the lines no parent has, which the
    merge itself wrote. What upstream brings in was not written here."""
    each = [added_lines(patch) for patch in patches]
    merged = {}
    for path, (numbers, text) in each[0].items():
        others = [set(other[path][1].split('\n')) for other in each[1:] if path in other]
        if len(others) == len(each) - 1:
            kept = [(number, line) for number, line in zip(numbers, text.split('\n'))
                    if all(line in other for other in others)]
            merged[path] = ([number for number, _ in kept], '\n'.join(line for _, line in kept))
    return merged


def raw_changes(output):
    """{path: (mode, object id)} of added, copied, modified and renamed files in git's --raw -z output."""
    fields, changes, i = output.split(b'\0'), {}, 0
    while i < len(fields) and fields[i]:
        meta = fields[i].decode().lstrip(':').split()
        status = meta[4][0]
        path = fields[i + (2 if status in 'RC' else 1)].decode('utf-8', 'replace')
        i += 3 if status in 'RC' else 2
        if status in 'ACMR' and meta[1] != '160000':
            changes[path] = (meta[1], meta[3])
    return changes


class Check:
    def __init__(self):
        self.text_rules = PATH_RULES + local_rules() + SECRET_RULES
        self.findings = []
        self.blobs = None

    def add(self, where, line, reason, excerpt=''):
        excerpt = excerpt.splitlines()[0][:100] if excerpt else ''
        if reason in SECRET_REASONS:
            excerpt = excerpt[:8] + '...'
        finding = (where, line, reason, excerpt)
        if finding not in self.findings:
            self.findings.append(finding)

    def path(self, path, rules, allowed=ALLOWED, where=None):
        if any(fnmatch.fnmatch(path, glob) for glob in allowed):
            return
        for pattern, reason in rules:
            if pattern.search(path):
                self.add(where or path, None, reason)

    def text(self, where, text, numbers=None, binary=False):
        """Paths and credentials in text; numbers holds the file's line number of each line of text.
        At most 20 hits of a rule per file: a log full of paths needs no more."""
        for pattern, reason, keep in self.text_rules:
            if binary and reason in NOT_IN_BINARIES:
                continue
            index, last, hits = 0, 0, 0
            for match in pattern.finditer(text):
                if keep and not keep(match):
                    continue
                start = text.rfind('\n', 0, match.start()) + 1
                end = text.find('\n', match.end())
                if ALLOW_MARK in text[start:end if end >= 0 else None]:
                    continue
                index += text.count('\n', last, match.start())
                last = match.start()
                line = None if binary else numbers[index] if numbers else index + 1
                self.add(where, line, reason, match.group())
                hits += 1
                if hits == 20:
                    break

    def content(self, where, path, data, magic, added=None, commit=False):
        if any(fnmatch.fnmatch(path, allowed) for allowed in ALLOWED):
            magic, commit = [], False
        for signature, reason in magic:
            if data.startswith(signature):
                self.add(where, None, reason)
        if commit and len(data) > MAX_FILE:
            self.add(where, None, f'file of {len(data) >> 20} MiB (game data?)')
        if b'\0' in data[:8000]:
            if commit and Path(path).suffix.lower() not in SAFE_BINARY and not data.startswith(
                    tuple(signature for signature, _ in magic)):
                self.add(where, None, 'binary file of an unknown kind (game data?)')
            strings = [match.group().decode('ascii') for match in ASCII_RUNS.finditer(data)]
            strings += [match.group().decode('utf-16-le') for match in UTF16_RUNS.finditer(data)]
            self.text(where, '\n'.join(strings), binary=True)
        elif added is not None:
            self.text(where, added[1], added[0])
        else:
            self.text(where, data.decode('utf-8', 'replace'))

    def changes(self, label, changes, added):
        """Files a commit (or the index) adds or changes, {path: (mode, object id)}, and their added lines."""
        for path, (_, oid) in changes.items():
            self.path(path, COMMIT_PATHS)
            data = self.blobs.read(oid)
            if data is not None:
                self.content(f'{label}{path}', path, data, COMMIT_MAGIC, added.get(path), commit=True)

    def commit(self, sha):
        self.text(f'{sha[:10]} message', git('log', '-1', '--format=%B', sha).decode('utf-8', 'replace'))
        parents = git('rev-list', '--parents', '-n', '1', sha).decode().split()[1:]
        raw = ['-r', '-z', '--raw', '--no-abbrev', '-M', '--diff-filter=ACMR']
        patch = ['-r', '-p', '-U0', '--no-color', '--no-ext-diff', '--no-textconv', '--no-prefix', '-M',
                 '--diff-filter=ACMR']
        if len(parents) > 1:
            self.merge(f'{sha[:10]} ', lambda *args: git('diff-tree', *args), [[parent, sha] for parent in parents],
                       raw, patch)
            return
        root = [] if parents else ['--root']
        changes = raw_changes(git('diff-tree', '--no-commit-id', *root, *raw, sha))
        self.changes(f'{sha[:10]} ', changes, added_lines(git('diff-tree', '--no-commit-id', *root, *patch, sha)))

    def staged(self):
        raw = ['--cached', '-z', '--raw', '--no-abbrev', '-M', '--diff-filter=ACMR']
        patch = ['--cached', '-U0', '--no-color', '--no-ext-diff', '--no-textconv', '--no-prefix', '-M',
                 '--diff-filter=ACMR']
        merging = git('rev-parse', '-q', '--verify', 'MERGE_HEAD', check=False).decode().split()
        if merging:
            # Committing a merge after conflicts: the index against HEAD and against the merged commit.
            self.merge('', lambda *args: git('diff', *args), [[], *([commit] for commit in merging)], raw, patch)
        else:
            self.changes('', raw_changes(git('diff', *raw)), added_lines(git('diff', *patch)))

    def merge(self, label, diff, bases, raw, patch):
        """A merge, diffed against each parent: only files that differ from every parent count, and in them
        only lines no parent has."""
        each = [raw_changes(diff(*raw, *base)) for base in bases]
        changes = {path: change for path, change in each[0].items() if all(path in other for other in each[1:])}
        if changes:
            paths = ['--', *changes] if len(changes) <= 100 else []
            self.changes(label, changes, merge_lines([diff(*patch, *base, *paths) for base in bases]))

    def tree(self):
        for entry in git('ls-files', '-s', '-z').split(b'\0'):
            if not entry:
                continue
            meta, path = entry.decode('utf-8', 'replace').split('\t', 1)
            mode, oid = meta.split()[:2]
            if mode == '160000':
                continue
            self.path(path, COMMIT_PATHS)
            data = self.blobs.read(oid)
            if data is not None:
                self.content(path, path, data, COMMIT_MAGIC, commit=True)

    def package(self, where, path, data, depth=0):
        """A file of a release: its path inside the package, its content, and zip-like files in it."""
        if path:
            self.path(path, PACKAGE_PATHS, PACKAGE_ALLOWED, where)
        if data.startswith(b'PK\x03\x04') and (not path or Path(path).suffix.lower() in ZIP_LIKE) and depth < 3:
            with zipfile.ZipFile(io.BytesIO(data)) as archive:
                for info in archive.infolist():
                    if not info.is_dir():
                        self.package(f'{where}!/{info.filename}', info.filename, archive.read(info), depth + 1)
            return
        self.content(where, path, data, GAME_MAGIC)

    def report(self):
        for where, line, reason, excerpt in self.findings[:200]:
            location = f'{where}:{line}' if line else where
            print(f'  {location}: {reason}' + (f': {excerpt}' if excerpt else ''), file=sys.stderr)
        if len(self.findings) > 200:
            print(f'  ... and {len(self.findings) - 200} more', file=sys.stderr)


def pushed_commits(lines):
    """The commits of a push (git's pre-push input) that no remote-tracking branch has yet."""
    commits = []
    for line in lines:
        fields = line.split()
        if len(fields) != 4 or fields[1] == ZERO:
            continue
        exclude = ['--remotes']
        if fields[3] != ZERO and subprocess.run(['git', 'cat-file', '-e', fields[3] + '^{commit}'], cwd=ROOT,
                                                capture_output=True).returncode == 0:
            exclude.append(fields[3])
        listed = git('rev-list', '--reverse', fields[1], '--not', *exclude).decode().split()
        commits += [sha for sha in listed if sha not in commits]
    return commits


def main():
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding='utf-8', errors='replace')
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('mode', choices=['staged', 'message', 'pre-push', 'commits', 'tree', 'package'])
    parser.add_argument('args', nargs=argparse.REMAINDER)
    options = parser.parse_args()
    if options.mode in ('message', 'package') and not options.args:
        parser.error(f'{options.mode} needs a file')
    check = Check()
    try:
        if options.mode == 'package':
            for item in map(Path, options.args):
                if item.is_dir():
                    for file in sorted(item.rglob('*')):
                        if file.is_file():
                            inside = f'{item.name}/{file.relative_to(item).as_posix()}'
                            check.package(inside, inside, file.read_bytes())
                else:
                    check.package(item.name, '', item.read_bytes())
        elif options.mode == 'message':
            # What git keeps of the message: no # comments, nothing below the scissors line of commit -v.
            lines = Path(options.args[0]).read_text(encoding='utf-8', errors='replace').split('\n')
            kept = []
            for number, line in enumerate(lines, 1):
                if line.startswith('# ') and line.endswith(' >8 ------------------------'):
                    break
                if not line.startswith('#'):
                    kept.append((number, line))
            check.text('commit message', '\n'.join(line for _, line in kept), [number for number, _ in kept])
        else:
            check.blobs = Blobs()
            if options.mode == 'staged':
                check.staged()
            elif options.mode == 'tree':
                check.tree()
            else:
                if options.mode == 'pre-push':
                    commits = pushed_commits(sys.stdin.read().splitlines())
                else:
                    commits = git('rev-list', '--reverse', *options.args).decode().split()
                for sha in commits:
                    check.commit(sha)
    except (OSError, RuntimeError, zipfile.BadZipFile) as error:
        print(f'leak-check: could not check: {error}', file=sys.stderr)
        return 2
    if not check.findings:
        return 0
    what, fix = {
        'staged': ('Commit refused', 'Unstage them: git restore --staged PATH (.gitignore keeps most out of git add).'),
        'message': ('Commit refused', 'Reword the message (git kept it in .git/COMMIT_EDITMSG).'),
        'pre-push': ('Push refused', 'Take them out of those commits (git commit --amend, git rebase) first.'),
        'commits': ('Found', 'Take them out of those commits (git commit --amend, git rebase).'),
        'tree': ('Found', 'These are tracked already.'),
        'package': ('Package refused', 'Leave them out of the package.'),
    }[options.mode]
    print(f'leak-check: {what}: game files, local paths or credentials:', file=sys.stderr)
    check.report()
    print(f'{fix} If one is safe, mark its line with "leak-check: allow" or list the file in ALLOWED in '
          'tools/leak_check.py.', file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main())
