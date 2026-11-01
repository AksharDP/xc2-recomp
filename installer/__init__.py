"""The XCX installer: builds a playable Xenoblade Chronicles X: Definitive Edition folder from the player's own game
files (docs/installer.md).

    python -m installer --base FILE --update FILE --keys FILE [--dlc FILE ...] --lang CODE --dest DIR [--root DIR]
                        [--temp DIR]

Modules: layout (where the bundled tools are, release or repository), events (JSON progress on stdout and the log
file), state (xcx-install.json), runner (which steps run: resume and update), steps (the seven steps), ini (xcx.ini),
cli (the command line). text/en.json holds the English text of every message id the events carry.

Standard library only, apart from what the extraction tools import (cryptography). Runs on Python 3.14.
"""
