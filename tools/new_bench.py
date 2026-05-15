#!/usr/bin/env python3
import argparse
import os
import re
import shutil
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
TEMPLATE_DIR = PROJECT_ROOT / 'benches' / '_template'
BENCHES_DIR  = PROJECT_ROOT / 'benches'

KNOWN_CATEGORIES = ['cell', 'xdr', 'ppe', 'disk', 'dma', 'fabric', 'workload', 'burn']

def fail(msg: str) -> None:
    print(f'error: {msg}', file=sys.stderr)
    sys.exit(1)


def valid_id(name: str) -> bool:
    return bool(re.match(r'^[a-z][a-z0-9_]*$', name))


def replace_in_file(path: Path, src: str, dst: str) -> None:
    text = path.read_text(encoding='utf-8')
    text = text.replace(src, dst)
    text = text.replace(src.upper(), dst.upper())
    text = text.replace(src.capitalize(), dst.capitalize())
    path.write_text(text, encoding='utf-8')


def main() -> int:
    parser = argparse.ArgumentParser(description='Scaffold a new cellmark benchmark from the template')
    parser.add_argument('name', help='Bench id (lowercase snake_case, ' 'matches benches/<name>/)')
    parser.add_argument('--category', default='workload', help=f'UI category. Common choices: ' f'{", ".join(KNOWN_CATEGORIES)} (default: workload)')
    parser.add_argument('--no-spu', action='store_true', help='Drop spu_main.c (PPE-only benchmark)')
    parser.add_argument('--force', action='store_true', help='Overwrite an existing benches/<name>/ directory')
    args = parser.parse_args()

    name = args.name.lower()
    if not valid_id(name):
        fail(f"'{name}' is not a valid bench id "
             f"(lowercase, starts with letter, [a-z0-9_] only)")
    if name in ('template', '_template'):
        fail("'template' is reserved; pick a different name")

    target = BENCHES_DIR / name
    if target.exists():
        if args.force:
            shutil.rmtree(target)
        else:
            fail(f'{target} already exists (use --force to overwrite)')

    if not TEMPLATE_DIR.exists():
        fail(f'template not found at {TEMPLATE_DIR}')

    print(f'[new_bench] scaffolding benches/{name}/ from _template/')
    shutil.copytree(TEMPLATE_DIR, target)

    if args.no_spu:
        spu_main = target / 'spu_main.c'
        if spu_main.exists():
            spu_main.unlink()
            print('[new_bench] dropped spu_main.c (--no-spu)')

    for f in target.rglob('*'):
        if f.is_file() and f.suffix in ('.c', '.h', '.S', '.md'):
            replace_in_file(f, 'template', name)

    bench_c = target / 'bench.c'
    if bench_c.exists():
        text = bench_c.read_text(encoding='utf-8')
        text = re.sub(r'\.category\s*=\s*"workload"', f'.category     = "{args.category}"', text)
        bench_c.write_text(text, encoding='utf-8')
    return 0

if __name__ == '__main__':
    sys.exit(main())