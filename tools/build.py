#!/usr/bin/env python3
"""
cellmark build driver

Usage:
    python tools/build.py                 # retail variant (default)
    python tools/build.py retail
    python tools/build.py decr            # DECR variant (libperf linked)
    python tools/build.py clean           # delete build artifacts
    python tools/build.py spu_stress      # rebuild just one SPU bench
    python tools/build.py ppu             # rebuild PPU only (assumes SPU obj exist)

Outputs land in build/:
    build/cellmark.elf
    build/cellmark.self            (retail variant only, fself)
    build/spu_elfs/<bench>.elf
    build/obj/spu_<bench>.ppu.o
    build/obj/cellmark_decr.elf    (decr variant)
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR    = PROJECT_ROOT / 'build'
OBJ_DIR      = BUILD_DIR / 'obj'
SPU_ELF_DIR  = BUILD_DIR / 'spu_elfs'

def _normalize_path(s: str) -> str:
    if len(s) >= 3 and s[0] == '/' and s[2] == '/' and s[1].isalpha():
        return s[1] + ':/' + s[3:]
    return s


CELL_SDK   = Path(_normalize_path(os.environ.get('CELL_SDK', r'C:\usr\local\cell')))
CELL_HOST  = CELL_SDK / 'host-win32'
PPU_CC     = CELL_HOST / 'ppu' / 'bin' / 'ppu-lv2-gcc.exe'
PPU_OBJCOPY= CELL_HOST / 'ppu' / 'bin' / 'ppu-lv2-objcopy.exe'
PPU_NM     = CELL_HOST / 'ppu' / 'bin' / 'ppu-lv2-nm.exe'
SPU_CC     = CELL_HOST / 'spu' / 'bin' / 'spu-lv2-gcc.exe'
PPU_TARGET = CELL_SDK / 'target' / 'ppu'
SPU_TARGET = CELL_SDK / 'target' / 'spu'
DBGFONT_LIB= PPU_TARGET / 'lib' / 'libdbgfont_gcm.a'

INCLUDE_DIR = PROJECT_ROOT / 'include'
PPU_INCLUDE = PROJECT_ROOT / 'ppu'
ENGINE_DIR  = PROJECT_ROOT / 'engine'
BENCHES_DIR = PROJECT_ROOT / 'benches'
SHARED_SPU  = ENGINE_DIR / 'spu_shared'

def find_make_fself() -> Path | None:
    for name in ('make_fself.exe', 'make_fself_npdrm.exe'):
        candidate = CELL_HOST / 'bin' / name
        if candidate.exists():
            return candidate
    return None

SPU_BENCHES: dict[str, list[str]] = {
    'stress':  ['benches/cell/spu/spu_stress.c',
                'benches/cell/spu/spu_memtest.c',
                'benches/cell/spu/spu_fma_kernel.S',
                'benches/cell/spu/spu_dp_fma_kernel.S',
                'benches/cell/spu/spu_int_kernel.S',
                'benches/cell/spu/spu_recip_kernel.S',
                'benches/cell/spu/spu_shuf_kernel.S',
                'engine/spu_shared/spu_dual_kernel.S'],
    'dma':     ['benches/dma/spu_dma_main.c',
                'engine/spu_shared/spu_dma_kernel.c'],
    'eib':     ['benches/eib/spu_eib_main.c',
                'engine/spu_shared/spu_dma_kernel.c'],
    'atomic':  ['benches/atomic/spu_atomic_main.c'],
    'mbox':    ['benches/mbox/spu_mbox_main.c'],
    'branch':  ['benches/branch/spu_branch_main.c',
                'benches/branch/spu_branch_kernel.S'],
    'pi':      ['benches/pi/spu_pi_main.c',
                'benches/pi/spu_pi_kernel.c'],
    'fft':     ['benches/fft/spu_fft_main.c',
                'benches/fft/spu_fft_kernel.c'],
    'nbody':   ['benches/nbody/spu_nbody_main.c',
                'benches/nbody/spu_nbody_kernel.c'],
    'burn':    ['benches/burn/spu_burn_main.c',
                'engine/spu_shared/spu_dual_kernel.S'],
}

PPU_SOURCES: list[str] = [
    'ppu/main.c',
    'engine/cellmark_engine.c',
    'engine/bench_modules.c',
    'engine/bench_registry.c',
    'engine/gcm.c', 'engine/spu.c', 'engine/render.c',
    'engine/pmu.c', 'engine/cell_pmu.c', 'engine/sysmon.c',
    'benches/cell/render_cell.c',
    'benches/ppe/ppe_benchmarks.c',  'benches/ppe/render_ppe.c',
    'benches/disk/disk_benchmark.c', 'benches/disk/render_disk.c',
    'benches/dma/dma_benchmark.c',   'benches/dma/render_dma.c',
    'benches/eib/eib_benchmark.c',   'benches/eib/render_eib.c',
    'benches/atomic/atomic_benchmark.c',
    'benches/mbox/mbox_benchmark.c',
    'benches/branch/branch_benchmark.c',
    'benches/pi/pi_benchmark.c',
    'benches/pi/spu_pi_kernel.c',    # cross-compiled for PPU self-test
    'benches/fft/fft_benchmark.c',
    'benches/nbody/nbody_benchmark.c',
    'benches/workload/render_workload.c',
    'benches/burn/burn_benchmark.c', 'benches/burn/render_burn.c',
]

PPU_LIBS_RETAIL = [
    '-lgcm_cmdasm', '-lgcm_sys_stub', '-lsysutil_stub',
    '-lsysmodule_stub', '-lio_stub', '-lfs_stub', '-lm',
]

PPU_LIBS_DECR_EXTRA = ['-lperf']

PPU_CFLAGS_BASE = [
    '-W', '-Wall', '-O3', '-g',
    '-m64', '-maltivec', '-mfused-madd', '-funroll-loops',
]
SPU_CFLAGS_BASE = ['-W', '-Wall', '-O3', '-g', '-funroll-loops']

SPU_CFLAGS_OVERRIDES: dict[str, list[str]] = {
    'stress': ['-W', '-Wall', '-O2', '-g', '-funroll-loops'],
}

SPU_LIBS: dict[str, list[str]] = {
    'fft': ['-lm'],
}

class BuildError(Exception):
    pass


def info(msg: str) -> None:
    print(f'[build] {msg}', flush=True)


def warn(msg: str) -> None:
    print(f'[build] WARNING: {msg}', file=sys.stderr, flush=True)


def fail(msg: str) -> 'NoReturn':
    print(f'[build] ERROR: {msg}', file=sys.stderr, flush=True)
    sys.exit(1)


def run(cmd: list[str | os.PathLike], cwd: Path | None = None) -> None:
    str_cmd = [str(c) for c in cmd]
    echo = str_cmd[0]
    if len(str_cmd) > 1:
        echo += ' ' + ' '.join(
            Path(a).name if Path(a).is_absolute() else a for a in str_cmd[1:]
        )
    if len(echo) > 200:
        echo = echo[:200] + ' ...'
    print(f'  $ {echo}', flush=True)
    try:
        subprocess.run(str_cmd, cwd=cwd, check=True)
    except subprocess.CalledProcessError as e:
        raise BuildError(f'command failed (exit {e.returncode}): {str_cmd[0]}')


def ensure_dir(p: Path) -> None:
    p.mkdir(parents=True, exist_ok=True)


def sanity_check_toolchain() -> None:
    for path, label in [
        (PPU_CC, 'PPU compiler'),
        (SPU_CC, 'SPU compiler'),
        (PPU_OBJCOPY, 'PPU objcopy'),
        (INCLUDE_DIR / 'stress_common.h', 'stress_common.h'),
    ]:
        if not Path(path).exists():
            fail(f'{label} not found at {path}')

def build_spu_bench(name: str, sources: list[str], build_define: list[str]) -> Path:
    info(f'SPU build: {name} ({len(sources)} source{"s" if len(sources)!=1 else ""})')
    out_elf = SPU_ELF_DIR / f'{name}.elf'
    flags = SPU_CFLAGS_OVERRIDES.get(name, SPU_CFLAGS_BASE)
    extra_libs = SPU_LIBS.get(name, [])
    spu_includes = sorted({f'-I{(PROJECT_ROOT / s).parent}' for s in sources})
    cmd: list[str | os.PathLike] = [
        SPU_CC, *flags, *build_define,
        f'-I{SPU_TARGET / "include"}',
        f'-I{INCLUDE_DIR}',
        f'-I{ENGINE_DIR}',     # pmu.h etc. are now in engine/
        f'-I{SHARED_SPU}',
        *spu_includes,
        '-o', out_elf,
        *[PROJECT_ROOT / s for s in sources],
        *extra_libs,
    ]
    run(cmd)
    return out_elf


def embed_spu_elf(name: str, elf_path: Path) -> Path:
    info(f'EMBED: {name}')
    embed_input = OBJ_DIR / f'spu_{name}.elf'
    embed_output = OBJ_DIR / f'spu_{name}.ppu.o'
    shutil.copyfile(elf_path, embed_input)

    cmd: list[str | os.PathLike] = [
        PPU_OBJCOPY,
        '-I', 'binary',
        '-O', 'elf64-powerpc-celloslv2',
        '-B', 'powerpc:common64',
        embed_input.name,
        embed_output.name,
    ]
    run(cmd, cwd=OBJ_DIR)

    # dump symbols for visibility, matching old build.bat output
    try:
        result = subprocess.run([str(PPU_NM), str(embed_output)],
                                capture_output=True, text=True, check=True)
        for line in result.stdout.splitlines():
            print(f'    {line}')
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass

    embed_input.unlink(missing_ok=True)
    return embed_output


def build_all_spu(build_define: list[str]) -> dict[str, Path]:
    embedded: dict[str, Path] = {}
    for name, sources in SPU_BENCHES.items():
        elf = build_spu_bench(name, sources, build_define)
        embedded[name] = embed_spu_elf(name, elf)
    return embedded


def link_ppu(variant: str, embedded_objs: dict[str, Path]) -> Path:
    if variant == 'decr':
        out_name = 'cellmark_decr.elf'
        build_define = ['-DCELLMARK_DECR=1']
        libs = PPU_LIBS_RETAIL + PPU_LIBS_DECR_EXTRA
    else:
        out_name = 'cellmark.elf'
        build_define = []
        libs = PPU_LIBS_RETAIL
    out_elf = BUILD_DIR / out_name

    bench_includes = [f'-I{BENCHES_DIR / d.name}'
                      for d in sorted(BENCHES_DIR.iterdir()) if d.is_dir()]

    info(f'PPU link: {out_name}')
    cmd: list[str | os.PathLike] = [
        PPU_CC, *PPU_CFLAGS_BASE, *build_define,
        f'-I{PPU_TARGET / "include"}',
        f'-I{INCLUDE_DIR}',
        f'-I{PPU_INCLUDE}',
        f'-I{ENGINE_DIR}',
        *bench_includes,
        f'-L{PPU_TARGET / "lib"}',
        '-o', out_elf,
        *[PROJECT_ROOT / s for s in PPU_SOURCES],
        *embedded_objs.values(),
        DBGFONT_LIB,
        *libs,
    ]
    run(cmd)
    return out_elf


def make_self(elf_path: Path) -> Path | None:
    make_fself = find_make_fself()
    if not make_fself:
        warn('make_fself not found; skipping SELF creation')
        return None
    out_self = elf_path.with_suffix('.self')
    info(f'SELF: {out_self.name}')
    run([make_fself, elf_path, out_self])
    return out_self

def cmd_clean() -> None:
    info('cleaning build/')
    if BUILD_DIR.exists():
        shutil.rmtree(BUILD_DIR)
    legacy = [PROJECT_ROOT / 'obj',
              PROJECT_ROOT / 'cellmark.elf',
              PROJECT_ROOT / 'cellmark.self',
              PROJECT_ROOT / 'cellmark_decr.elf',
              PROJECT_ROOT / 'cellmark_decr.pkg']
    for f in legacy:
        if f.is_dir():
            shutil.rmtree(f)
        elif f.exists():
            f.unlink()
    for elf in (PROJECT_ROOT / 'spu').glob('spu_*.elf'):
        elf.unlink()


def cmd_build(variant: str, only: str | None = None) -> None:
    sanity_check_toolchain()
    ensure_dir(BUILD_DIR)
    ensure_dir(OBJ_DIR)
    ensure_dir(SPU_ELF_DIR)

    info(f'variant: {variant.upper()}')
    info(f'project root: {PROJECT_ROOT}')
    info(f'CELL SDK:     {CELL_SDK}')

    build_define = ['-DCELLMARK_DECR=1'] if variant == 'decr' else []

    if only and only != 'ppu':
        if only not in SPU_BENCHES:
            fail(f'unknown SPU bench: {only} (known: {", ".join(SPU_BENCHES)})')
        elf = build_spu_bench(only, SPU_BENCHES[only], build_define)
        embed_spu_elf(only, elf)
        return

    if only == 'ppu':
        embedded = {n: OBJ_DIR / f'spu_{n}.ppu.o' for n in SPU_BENCHES}
        for p in embedded.values():
            if not p.exists():
                fail(f'PPU-only build requested but {p} is missing; '
                     f'run a full build first')
    else:
        embedded = build_all_spu(build_define)

    elf = link_ppu(variant, embedded)

    self_path = make_self(elf)

    info('')
    info('=' * 60)
    info(f'BUILD COMPLETE  (variant: {variant})')
    info('=' * 60)
    info(f'  ELF:  {elf}')
    if self_path and self_path.exists():
        info(f'  SELF: {self_path}')
        if variant == 'retail':
            info(f'  Retail: transfer {self_path.name} via FTP or USB')
        else:
            info(f'  DECR:   load {self_path.name} via Target Manager / ProDG')
    elif variant == 'decr':
        info(f'  DECR:   load {elf.name} via Target Manager / ProDG (unsigned)')


def main() -> int:
    parser = argparse.ArgumentParser(description='cellmark build driver')
    parser.add_argument('args', nargs='*',
                        help='[variant] [clean|<bench>|ppu]')
    ns = parser.parse_args()

    args = list(ns.args)
    variant = 'retail'
    if args and args[0].lower() in ('retail', 'decr'):
        variant = args.pop(0).lower()
    variant = os.environ.get('CELLMARK_BUILD', variant).lower()

    if args and args[0] == 'clean':
        cmd_clean()
        return 0

    only = args[0] if args else None

    try:
        cmd_build(variant, only)
    except BuildError as e:
        fail(str(e))
    return 0


if __name__ == '__main__':
    sys.exit(main())