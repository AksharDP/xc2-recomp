"""Remember the installer's last form values outside the unpacked release and install folders."""
import json
import os
import sys
import tempfile
from pathlib import Path

FORMAT = 1


def config_path():
    """The per-user launcher config, retained when a new setup release is unpacked elsewhere."""
    if os.name == 'nt':
        root = os.environ.get('LOCALAPPDATA') or os.environ.get('APPDATA')
        if root:
            return Path(root) / 'XC2 Recomp' / 'launcher.json'
        return Path.home() / 'AppData' / 'Local' / 'XC2 Recomp' / 'launcher.json'
    if sys.platform == 'darwin':
        return Path.home() / 'Library' / 'Application Support' / 'XC2 Recomp' / 'launcher.json'
    root = os.environ.get('XDG_CONFIG_HOME')
    return (Path(root) if root else Path.home() / '.config') / 'xc2-recomp' / 'launcher.json'


def defaults():
    """Return empty values for the installer's remembered fields. check_updates: whether the window asks GitHub for a
    newer setup when it opens (installer/selfupdate.py); "check_updates": false in the file turns that off."""
    return {'base': '', 'update': '', 'dlc': [], 'keys': '', 'dest': '', 'check_updates': True}


def load(target=None):
    """Load saved paths; a missing, invalid or newer config starts with blank values."""
    path = Path(target) if target is not None else config_path()
    try:
        data = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return defaults()
    if not isinstance(data, dict) or data.get('format') != FORMAT:
        return defaults()

    values = defaults()
    for name in ('base', 'update', 'keys', 'dest'):
        value = data.get(name)
        if isinstance(value, str):
            values[name] = value
    dlc = data.get('dlc')
    if isinstance(dlc, list):
        values['dlc'] = [path for path in dlc if isinstance(path, str)]
    if isinstance(data.get('check_updates'), bool):
        values['check_updates'] = data['check_updates']
    return values


def save(values, target=None):
    """Write preferences atomically using a unique temporary file; failures must not block an install."""
    path = Path(target) if target is not None else config_path()
    data = {'format': FORMAT}
    for name in ('base', 'update', 'keys', 'dest'):
        value = values.get(name, '')
        data[name] = value if isinstance(value, str) else ''
    dlc = values.get('dlc', ())
    data['dlc'] = [item for item in dlc if isinstance(item, str)] if isinstance(dlc, (list, tuple)) else []
    data['check_updates'] = values.get('check_updates', True) is not False
    temporary = None
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile('w', encoding='utf-8', newline='\n', prefix=path.name + '.',
                                         suffix='.tmp', dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(data, stream, indent=1, ensure_ascii=False)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    except (OSError, UnicodeError):
        if temporary is not None:
            try:
                temporary.unlink()
            except OSError:
                pass
