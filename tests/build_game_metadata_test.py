"""Check the game identity emitted by the native build without game inputs."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import build_game


class GameNameTest(unittest.TestCase):
    def test_game_name_survives_module_and_path_loops(self):
        paths = {key: Path('test') / key for key in ('exefs', 'romfs', 'profile')}
        layout = [('main', 0x7000000000, 0x1000), ('sdk', 0x7000100000, 0x1000)]
        cases = [('v301', None, 'Xenoblade Chronicles 2 3.0.1'),
                 ('v301', 'Xenoblade Chronicles 2 3.0.1 (Japan)', 'Xenoblade Chronicles 2 3.0.1 (Japan)'),
                 ('custom', None, '')]
        for version, name, expected in cases:
            with self.subTest(version=version, name=name):
                glue = build_game.module_glue(layout, ['main', 'sdk'], {}, paths, version,
                                             0x0100e95004038000, name=name)
                self.assertIn(f'const char rt_game_name[] = "{expected}";', glue)
                for key, path in paths.items():
                    self.assertIn(f'const char rt_default_{key}[] = "{path.as_posix()}";', glue)


if __name__ == '__main__':
    unittest.main()
