"""tools/leak_check.py. The leaks below are assembled from pieces, so this file passes the check itself."""
from paths import ROOT
import io
import sys
import unittest
import zipfile

sys.path.insert(0, str(ROOT / 'tools'))
import leak_check  # noqa: E402

DRIVE = 'C:'
TOKEN = 'ghp_' + 'a' * 36


def check():
    result = leak_check.Check()
    result.text_rules = leak_check.PATH_RULES + leak_check.SECRET_RULES  # no rules of the machine running the tests
    return result


def reasons(result):
    return sorted({reason for _, _, reason, _ in result.findings})


class PathTests(unittest.TestCase):
    def refused(self, path, rules=leak_check.COMMIT_PATHS):
        result = check()
        result.path(path, rules)
        return bool(result.findings)

    def test_game_files_and_build_output_are_refused(self):
        for path in ['eboot.bin', 'game/big2-ps4_Shipping.elf', 'sce_module/libc.prx', 'x/sce_sys/param.sfo',
                     'u3data/bin/thing', 'CUSA02343-app0/readme.txt', 'out/eboot/boot-linked.bin',
                     'dist/uncharted-windows.zip', 'xbox/.local/settings.json', 'xbox/signing/test.pfx',
                     'user/savedata/SAVE.DAT', 'cache/pipeline.vkcache', 'bin/bb-probe.exe', '.env']:
            self.assertTrue(self.refused(path), path)

    def test_sources_and_examples_pass(self):
        for path in ['src/runtime_memory.c', 'scripts/prepare.py', 'launcher/uncharted.ico',
                     'xbox/settings.example.json', 'patches/CUSA02343.xml', 'tests/data/fixture.bin']:
            self.assertFalse(self.refused(path), path)

    def test_fsr4_weights_may_ship_but_not_be_committed(self):
        weights = 'uncharted-windows/fsr4_shaders/fsr4_model_v07_i8_quality_initializers.bin'
        result = check()
        result.path(weights, leak_check.PACKAGE_PATHS, leak_check.PACKAGE_ALLOWED)
        self.assertEqual(result.findings, [])
        self.assertTrue(self.refused('fsr4_shaders/model.bin'))


class TextTests(unittest.TestCase):
    def scan(self, text):
        result = check()
        result.text('file', text)
        return reasons(result)

    def test_user_profiles_and_dump_paths_are_refused(self):
        self.assertEqual(self.scan(f'log: {DRIVE}\\Users\\alice\\AppData'), ['user profile path'])
        self.assertEqual(self.scan(f'"{DRIVE}\\\\Users\\\\alice\\\\x"'), ['user profile path'])
        self.assertEqual(self.scan('/c/' + 'Users/alice/msys64'), ['user profile path'])
        self.assertEqual(self.scan('/home' + '/alice/games'), ['user profile path'])
        self.assertEqual(self.scan('H:' + '\\homebrew\\dumps\\CUSA02343-app0'), ['path into a game dump'])

    def test_placeholders_pass(self):
        for text in [f'{DRIVE}\\Users\\<you>\\AppData', f'{DRIVE}\\Users\\%USERNAME%', '/c/' + 'Users/$USER/x',
                     '/home' + '/runner/work', '/path/to/CUSA03173', 'D:' + '\\here\\CUSA02343',
                     'D:\\DevelopmentFiles\\dumps', f'{DRIVE}\\Users\\alice  # leak-check: allow',
                     'L"E:\\\\dumps\\\\CUSA02343-app0"', 'D:\\DevelopmentFiles\\dumps\\CUSA02343-app0']:
            self.assertEqual(self.scan(text), [], text)

    def test_credentials_are_refused_and_not_printed(self):
        result = check()
        result.text('config.py', f'token = "{TOKEN}"\npassword = "' + 'hunter22hunter' + '"\npassword = "<your password>"')
        self.assertEqual(reasons(result), ['GitHub token', 'hard-coded secret'])
        self.assertTrue(all(TOKEN not in excerpt for *_, excerpt in result.findings))

    def test_line_numbers_follow_the_file(self):
        result = check()
        result.text('notes.md', f'clean\n{DRIVE}\\Users\\alice', [10, 42])
        self.assertEqual(result.findings[0][1], 42)

    def test_this_machine_in_any_spelling(self):
        pattern = leak_check.path_pattern(DRIVE + '\\Projects\\uncharted_pc')
        for text in [DRIVE + '\\Projects\\uncharted_pc\\out', DRIVE + '/Projects/uncharted_pc',
                     DRIVE + '\\\\Projects\\\\uncharted_pc', '/c/' + 'Projects/uncharted_pc',
                     '/mnt/c/' + 'Projects/uncharted_pc']:
            self.assertTrue(pattern.search(text), text)
        self.assertFalse(pattern.search(DRIVE + '\\Projects\\uncharted_pc2'))


class ContentTests(unittest.TestCase):
    def test_binaries(self):
        result = check()
        result.content('eboot.bin', 'eboot.bin', b'\x7fELF\x02\x01\x01' + b'\0' * 64, leak_check.COMMIT_MAGIC, commit=True)
        result.content('a.dat', 'a.dat', b'\0\1\2', leak_check.COMMIT_MAGIC, commit=True)
        self.assertEqual(reasons(result), ['ELF executable (a game executable or module)',
                                           'binary file of an unknown kind (game data?)'])

    def test_package_nesting(self):
        appx = io.BytesIO()
        with zipfile.ZipFile(appx, 'w') as archive:
            archive.writestr('Prepared/boot-linked.bin', b'\0' * 16)
            archive.writestr('app.exe', b'MZ' + b'\0' * 32 + ('H:' + '\\homebrew\\dumps\\CUSA02343').encode())
        release = io.BytesIO()
        with zipfile.ZipFile(release, 'w') as archive:
            archive.writestr('Launcher_1.0.0.0_x64.appx', appx.getvalue())
        result = check()
        result.package('release.zip', '', release.getvalue())
        self.assertEqual({where for where, *_ in result.findings},
                         {'release.zip!/Launcher_1.0.0.0_x64.appx!/Prepared/boot-linked.bin',
                          'release.zip!/Launcher_1.0.0.0_x64.appx!/app.exe'})


class MergeTests(unittest.TestCase):
    def test_only_lines_the_merge_wrote(self):
        ours = b'+++ notes.md\n@@ -1,0 +2,2 @@\n+from upstream\n+written in the merge\n'
        theirs = b'+++ notes.md\n@@ -1,0 +1,2 @@\n+from the fork\n+written in the merge\n'
        self.assertEqual(leak_check.merge_lines([ours, theirs]), {'notes.md': ([3], 'written in the merge')})


if __name__ == '__main__':
    unittest.main()
