"""Git worktrees for agent tasks: .worktrees/<tag> on branch task/<tag>, linked to the main checkout's ignored data.

  worktree.py new <tag> [--base REF]       create the worktree and the links, print its path
  worktree.py list                         worktrees with branch, ahead/behind main, dirty or not
  worktree.py remove <tag> [--delete-branch] [--force]

A worktree holds tracked files only. build/, generated/, refs/, toolchain/, .venv/, .venv-py314/, game/, and the local
agent setup .claude/ and notes/ of the main checkout appear in it as directory junctions, config.local.toml and
CLAUDE.local.md as hard links. Nothing is copied.
`remove` deletes the links first (os.rmdir / os.unlink never follow a junction), checks that none is left, and only
then lets git delete the worktree. Stdlib only.
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

LINK_DIRS = ['build', 'generated', 'refs', 'toolchain', '.venv', '.venv-py314', 'game', '.claude', 'notes']
LINK_FILES = ['config.local.toml', 'CLAUDE.local.md']
MAIN_BRANCH = 'main'
TAG_RE = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]*$')


def git(cwd, *args, check=True):
    r = subprocess.run(['git', '-C', str(cwd), *args], capture_output=True, text=True)
    if check and r.returncode:
        sys.exit(f'git {" ".join(args)} failed: {(r.stderr or r.stdout).strip()}')
    return r


def main_checkout(start):
    """The main checkout: the parent of the shared .git directory, whichever worktree `start` is in."""
    common = git(start, 'rev-parse', '--path-format=absolute', '--git-common-dir').stdout.strip()
    return Path(common).resolve().parent


def is_link(p):
    """True for a junction or symlink."""
    return os.path.islink(p) or os.path.isjunction(p)


def make_junction(target, link):
    if os.name == 'nt':
        import _winapi
        _winapi.CreateJunction(str(target), str(link))
    else:
        os.symlink(target, link, target_is_directory=True)


def drop_link(p):
    """Remove one link without touching what it points at. A real directory or file is left alone."""
    if os.path.isjunction(p) or os.path.islink(p):
        os.rmdir(p) if os.path.isjunction(p) else os.unlink(p)
        return True
    return False


def link_data(main, wt):
    made = []
    for name in LINK_DIRS:
        src, dst = main / name, wt / name
        if not src.is_dir() or os.path.lexists(dst):
            continue
        make_junction(src, dst)
        made.append(name)
    for name in LINK_FILES:
        src, dst = main / name, wt / name
        if not src.is_file() or os.path.lexists(dst):
            continue
        os.link(src, dst)
        made.append(name)
    return made


def unlink_data(wt):
    """Remove the links of a worktree. A file link goes by os.unlink: it only drops one name of the data."""
    for name in LINK_DIRS:
        drop_link(wt / name)
    for name in LINK_FILES:
        p = wt / name
        if os.path.lexists(p) and not is_link(p):
            os.unlink(p)    # a hard link looks like a plain file; its data survives under the main checkout's name
    left = [n for n in LINK_DIRS if is_link(wt / n)]
    if left:
        sys.exit(f'links still present in {wt}: {left}; not removing the worktree')


def wt_path(main, tag):
    return main / '.worktrees' / tag


def cmd_new(a):
    if not TAG_RE.match(a.tag):
        sys.exit(f'bad tag {a.tag!r}')
    main = main_checkout(Path(__file__).resolve().parent)
    wt = wt_path(main, a.tag)
    if wt.exists():
        sys.exit(f'{wt} exists')
    git(main, 'worktree', 'add', str(wt), '-b', f'task/{a.tag}', a.base)
    made = link_data(main, wt)
    print(wt)
    print('linked: ' + ', '.join(made), file=sys.stderr)


def worktrees(main):
    out, cur = [], {}
    for line in git(main, 'worktree', 'list', '--porcelain').stdout.splitlines() + ['']:
        if not line:
            if cur:
                out.append(cur)
            cur = {}
        else:
            k, _, v = line.partition(' ')
            cur[k] = v
    return out


def cmd_list(a):
    main = main_checkout(Path(__file__).resolve().parent)
    for w in worktrees(main):
        path = Path(w['worktree'])
        branch = w.get('branch', '(detached)').removeprefix('refs/heads/')
        r = git(path, 'rev-list', '--left-right', '--count', f'{MAIN_BRANCH}...HEAD', check=False)
        behind, ahead = r.stdout.split() if r.returncode == 0 else ('?', '?')
        dirty = bool(git(path, 'status', '--porcelain', check=False).stdout.strip())
        print(f'{path}  {branch}  ahead {ahead} behind {behind}  {"dirty" if dirty else "clean"}')


def cmd_remove(a):
    main = main_checkout(Path(__file__).resolve().parent)
    wt = wt_path(main, a.tag)
    branch = f'task/{a.tag}'
    if not wt.is_dir():
        sys.exit(f'{wt} not found')
    if git(wt, 'status', '--porcelain').stdout.strip() and not a.force:
        sys.exit(f'{wt} has uncommitted changes; commit them or pass --force')
    if a.delete_branch and not a.force:
        merged = git(main, 'branch', '--merged', MAIN_BRANCH, '--format=%(refname:short)').stdout.split()
        if branch not in merged:
            sys.exit(f'{branch} is not merged into {MAIN_BRANCH}; pass --force to delete it anyway')
    unlink_data(wt)
    git(main, 'worktree', 'remove', *(['--force'] if a.force else []), str(wt))
    print(f'removed {wt}')
    if a.delete_branch:
        git(main, 'branch', '-D', branch)
        print(f'deleted branch {branch}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('new')
    p.add_argument('tag')
    p.add_argument('--base', default=MAIN_BRANCH)
    p.set_defaults(fn=cmd_new)
    sub.add_parser('list').set_defaults(fn=cmd_list)
    p = sub.add_parser('remove')
    p.add_argument('tag')
    p.add_argument('--delete-branch', action='store_true')
    p.add_argument('--force', action='store_true')
    p.set_defaults(fn=cmd_remove)
    a = ap.parse_args()
    a.fn(a)


if __name__ == '__main__':
    main()
