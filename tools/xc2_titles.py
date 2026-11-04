"""The releases of Xenoblade Chronicles 2 the tools know: one per application id.

Nintendo published XC2 under two application ids. The Americas and Europe share one (0100E95004038000): one title
has one set of base and update packages, so the hashes of tools/data/expected-xc2-<version>.json cover both. The
Japanese release (product code ADENA, also sold in Asia) has its own (0100F3400332C000) with its own code and data, so
its expected hashes live in tools/data/expected-xc2-jp-<version>.json and its build version names carry the suffix jp
(v301jp). A release whose expected files are missing is recognised and refused with its name.

Each release follows the console's numbering: its update is the application id + 0x800 and its add-on content the
application id + 0x1000 + the AddOnContentIndex (0x063 = 99 is the western Japanese Voice pack).

The installer (installer/steps.py), the extraction (tools/xc2_extract.py, tools/package_select.py), the mod scans
(tools/cheats.py, tools/exefs_patches.py) and the build (tools/build_game.py, which bakes the id into modules.c as
rt_title_id for runtime/fs_host.c, svc.c and mods.c) all read this table.
"""
from dataclasses import dataclass
from pathlib import Path

DATA = Path(__file__).resolve().parent / 'data'
AOC_OFFSET = 0x1000
UPDATE_OFFSET = 0x800
AOC_MAX = 2000   # the highest AddOnContentIndex taken for one of the game's add-ons


@dataclass(frozen=True)
class Region:
    key: str      # 'us' (the Americas and Europe) or 'jp'
    name: str     # what the installer shows
    app_id: int
    suffix: str   # '' or 'jp': the infix of the expected files and the suffix of the build version names

    @property
    def title(self):
        return f'{self.app_id:016X}'

    @property
    def update_title(self):
        return f'{self.app_id + UPDATE_OFFSET:016X}'

    @property
    def aoc_base(self):
        return self.app_id + AOC_OFFSET

    def aoc_index(self, title_id):
        """The AddOnContentIndex of an add-on title id of this release, else None."""
        index = int(title_id, 16) - self.aoc_base
        return index if 0 < index < AOC_MAX else None

    def expected_name(self, version):
        """The file name of the expected hashes of the base game or an update of this release."""
        return f'expected-xc2-{self.suffix}-{version}.json' if self.suffix else f'expected-xc2-{version}.json'

    def expected(self, version, data=DATA):
        return Path(data) / self.expected_name(version)

    def version_tag(self, version):
        """'3.0.1' -> 'v301' (or 'v301jp'): the build version name of tools/build_game.py (generated/ and build/)."""
        return 'v' + version.replace('.', '') + self.suffix


REGIONS = (
    Region('us', 'Americas and Europe', 0x0100E95004038000, ''),
    Region('jp', 'Japan', 0x0100F3400332C000, 'jp'),
)
DEFAULT = REGIONS[0]
BY_KEY = {r.key: r for r in REGIONS}
TITLES = tuple(r.title for r in REGIONS)


def of_title(title_id):
    """The release whose base or update title id this is, else None."""
    t = title_id.upper()
    return next((r for r in REGIONS if t in (r.title, r.update_title)), None)


def of_aoc(title_id):
    """(release, AddOnContentIndex) of an add-on title id, else None."""
    for r in REGIONS:
        index = r.aoc_index(title_id)
        if index is not None:
            return r, index
    return None


def of_tag(tag):
    """The release of a build version name ('v301' -> us, 'v301jp' -> jp)."""
    return next((r for r in REGIONS if r.suffix and tag.endswith(r.suffix)), DEFAULT)
