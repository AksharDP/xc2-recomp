"""tools/extract.py's reading of hactool's merge log: signature lines never fail a merge, hash checks do. The lines are
hactool's own format; no game files, no tools.

Run under .venv-py314: python tests/hactool_log_test.py
"""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
from extract import HACTOOL_ERROR, HACTOOL_HASH_FAIL, UNSIGNED  # noqa: E402

# What the Linux release's hactool prints for a genuine update (issue #30).
CLANG_BUILD = '''Fixed-Key Index (GOOD):             0x0
Fixed-Key Signature (FAIL):         <hex removed>
NPDM Signature (FAIL):              <hex removed>
    ACID:
        Signature Key (GOOD):       0
        Signature (FAIL):           <hex removed>
        Level 5 (GOOD):
'''


def checked(log):
    return '\n'.join(line for line in log.splitlines() if not UNSIGNED.match(line))


class HactoolLog(unittest.TestCase):
    def test_signatures_do_not_fail_a_merge(self):
        self.assertIsNone(HACTOOL_ERROR.search(checked(CLANG_BUILD)))

    def test_hash_failures_still_do(self):
        for line in ('        Level 3 (FAIL):', '    Superblock Hash (FAIL):', '    Hash Table (FAIL):'):
            log = checked(CLANG_BUILD + line + '\n')
            self.assertIsNotNone(HACTOOL_HASH_FAIL.search(log), line)
            self.assertIsNotNone(HACTOOL_ERROR.search(log), line)

    def test_other_errors_still_do(self):
        for line in ('Failed to open file', 'Error: section bounds', '    Signature Key (FAIL):       9'):
            self.assertIsNotNone(HACTOOL_ERROR.search(checked(CLANG_BUILD + line + '\n')), line)


if __name__ == '__main__':
    unittest.main()
