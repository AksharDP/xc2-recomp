"""Base game selection for cartridge dumps that bundle an update (a "combined" XCI).

The vendored program_nca() accepts one content set only. A combined XCI carries the base set and an update set (its
Meta NCA under the update title ID, its Program NCA a BKTR patch). base_records() drops the update set so the base
checks run unchanged; run() puts that in front of the vendored program_nca() for the vendored scripts it starts."""
import importlib
import runpy
from pathlib import Path

VENDOR = Path(__file__).resolve().parent / 'vendor' / 'macnativegames'
BASE_TITLE = '0100453019AA8000'


def is_bktr(hdr):
    return any(hdr[0x400 + i * 512 + 4] == 4 for i in range(4))


def base_records(records):
    """records minus a bundled update: Meta NCAs of another title and BKTR Program NCAs. A package with no base
    Meta NCA is left as it is, so an update or another game still fails the vendored checks."""
    kept = [(r, h) for r, h in records
            if not (r['content_type'] == 1 and r['title_id'] != BASE_TITLE)
            and not (r['content_type'] == 0 and is_bktr(h))]
    if not any(r['content_type'] == 1 for r, _ in kept):
        return records
    return kept


def program_nca(records, update=False):
    import extract_exefs
    return extract_exefs.program_nca(records if update else base_records(records), update=update)


def run(path):
    """Run the script at path as __main__. A vendored script is imported and its main() called instead, after its
    module's program_nca is replaced: runpy would define a fresh, unreplaced program_nca in __main__."""
    path = Path(path).resolve()
    if path.parent != VENDOR:
        runpy.run_path(str(path), run_name='__main__')
        return
    import extract_exefs
    vendor_program_nca = extract_exefs.program_nca
    extract_exefs.program_nca = lambda records, update=False: vendor_program_nca(
        records if update else base_records(records), update=update)
    importlib.import_module(path.stem).main()
