"""The XC2 installer: builds a playable Xenoblade Chronicles 2 folder from the player's own game
files (docs/installer.md).

    python -m installer --base FILE --update FILE --keys FILE [--dlc FILE ...] --lang CODE --dest DIR [--root DIR]
                        [--temp DIR]

Modules: layout (where the bundled tools are, release or repository), events (JSON progress on stdout and the log
file), state (xc2-install.json), runner (which steps run: resume and update), steps (the seven steps), ini (xc2.ini),
cli (the command line). text/en.json holds the English text of every message id the events carry.

Standard library only, apart from what the extraction tools import (cryptography). Runs on Python 3.14.
"""
