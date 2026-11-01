"""xcx-install.json in the install folder: what built it and which steps finished.

    {"format": 1, "release": "...", "lang": "en-US",
     "inputs": {"base": {"name", "size", "sha256"}, "update": {...}},
     "steps": {"<step>": {"key": "<input fingerprint>", "release": "...", "finished": "<UTC time>", "seconds": 12.3,
                          ...the step's own fields}}}

A step's key fingerprints everything its output depends on (runner.py), so a step whose recorded key equals the
current one and whose output still checks out is not run again. The file is replaced atomically after each step.
No key value or keys file content is ever stored.
"""
import json
import os
from pathlib import Path

NAME = 'xcx-install.json'
FORMAT = 1


class StateError(Exception):
    def __init__(self, id, path):
        super().__init__(id)
        self.id, self.path = id, path


def empty(release):
    return {'format': FORMAT, 'release': release, 'inputs': {}, 'steps': {}}


def load(dest):
    """The state of dest, or None when the folder has none. StateError for an unreadable or newer file."""
    path = Path(dest) / NAME
    if not path.is_file():
        return None
    try:
        data = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        raise StateError('state.unreadable', path) from None
    if not isinstance(data, dict) or not isinstance(data.get('steps'), dict):
        raise StateError('state.unreadable', path)
    if data.get('format', 0) > FORMAT:
        raise StateError('state.newer_format', path)
    return data


def save(dest, data):
    path = Path(dest) / NAME
    tmp = path.with_name(NAME + '.tmp')
    with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(data, f, indent=1, ensure_ascii=False)
        f.write('\n')
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
