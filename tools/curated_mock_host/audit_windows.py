"""Bounded MSVC PE/map composition audit, not a security certification.

The linker map exposes public symbols (and any static symbols the linker lists),
not a complete inventory of all code or runtime-loaded modules.
"""
import argparse
import pathlib
import re
import subprocess
import sys

IMAGE = re.compile(r'^\s*File Type:\s*EXECUTABLE IMAGE\s*$', re.I | re.M)
SUMMARY = re.compile(r'^\s*Summary\s*$', re.I | re.M)
DLL = re.compile(r'^\s*([\w.-]+\.(?:dll|exe))\s*$', re.I | re.M)
MAP_SYMBOL = re.compile(r'^\s*([0-9A-Fa-f]{4}):([0-9A-Fa-f]+)\s+(\S+)\s+[0-9A-Fa-f]+\s+', re.M)
MAP_CODE = re.compile(r'^\s*([0-9A-Fa-f]{4}):([0-9A-Fa-f]+)\s+([0-9A-Fa-f]+)H\s+\S+\s+CODE\s*$', re.I | re.M)
ALLOWED_DLLS = {'kernel32.dll', 'vcruntime140.dll', 'vcruntime140_1.dll',
                'msvcp140.dll', 'ucrtbase.dll'}
CRT_DLL = re.compile(r'api-ms-win-crt-[a-z0-9-]+\.dll\Z')
LEGACY = re.compile(r'@(?:app|plugins|hook|host|game)@sco@|'
                    r'@(?:mock_game|net_sim)@hostd@scuba@|'
                    r'(?:^|[?@_])sco_(?:query|provide|raw|tasks|events|commands)', re.I)


def check(imports, exports, linkmap):
    for label, data in (('imports', imports), ('exports', exports)):
        if not IMAGE.search(data) or not SUMMARY.search(data):
            raise ValueError('unrecognized dumpbin ' + label + ' output')
    dlls = [name.lower() for name in DLL.findall(imports)]
    if not dlls:
        raise ValueError('no PE import descriptors parsed')
    unexpected = sorted({name for name in dlls if name not in ALLOWED_DLLS and not CRT_DLL.fullmatch(name)})
    if unexpected:
        raise ValueError('unexpected PE dependencies: ' + repr(unexpected))
    # A curated executable should have no exports at all. A table is rejected
    # regardless of spelling, aliases, ordinal-only exports, or decoration.
    if re.search(r'^\s*ordinal\s+hint\s+RVA\s+name\s*$', exports, re.I | re.M):
        raise ValueError('executable has an export table')
    if re.search(r'\b(?:exports|export table)\b', exports, re.I):
        raise ValueError('unrecognized export output')
    public_header = re.search(r'^\s*Address\s+Publics by Value\s+Rva\+Base', linkmap, re.I | re.M)
    if not public_header:
        raise ValueError('missing linker map public-symbol header')
    public_end = re.search(r'^\s*(?:entry point at|Static symbols|Summary)\b', linkmap[public_header.end():], re.I | re.M)
    if not public_end:
        raise ValueError('missing linker map public-symbol terminator')
    public = linkmap[public_header.end():public_header.end() + public_end.start()]
    code = [(int(seg, 16), int(start, 16), int(start, 16) + int(size, 16))
            for seg, start, size in MAP_CODE.findall(linkmap)]
    symbols = MAP_SYMBOL.findall(linkmap)
    public_symbols = MAP_SYMBOL.findall(public)
    if not code or not symbols or not public_symbols:
        raise ValueError('missing linker map code or symbol rows')
    authorities = [(int(seg, 16), int(offset, 16)) for seg, offset, name in public_symbols
                   if name.startswith('?CreateExecutableAuthority@reserved@sco@@')]
    if len(authorities) != 1 or not any(seg == s and start <= offset < end
                                         for seg, offset in authorities for s, start, end in code):
        raise ValueError('expected exactly one public code CreateExecutableAuthority definition')
    for _, _, name in symbols:
        if LEGACY.search(name) or 'ReaderDrainObserved' in name or 'reserved_copy_probe' in name:
            raise ValueError('unexpected legacy or test-only map symbol: ' + name)
    return sorted(set(dlls))


def selftest():
    im = 'File Type: EXECUTABLE IMAGE\n KERNEL32.dll\n VCRUNTIME140.dll\n api-ms-win-crt-runtime-l1-1-0.dll\n Summary\n'
    ex = 'File Type: EXECUTABLE IMAGE\n Summary\n'
    mp = (' 0001:00000000 00000100H .text CODE\n Address Publics by Value Rva+Base Lib:Object\n'
          ' 0001:00000010 ?CreateExecutableAuthority@reserved@sco@@YAXXZ 0000000140001010 f core.obj\n'
          ' entry point at 0001:00000020\n')
    check(im, ex, mp)
    bad = [(im + ' evil.dll\n', ex, mp), (im, ex + ' ordinal hint RVA name\n', mp),
           (im, ex, mp.replace(' 0001:00000010 ?', ' 0002:00000010 ?')),
           (im, ex, mp.replace(' entry point', ' 0001:00000030 ?ReaderDrainObserved@reserved@sco@@YAXXZ 0000000140001030 f core.obj\n entry point')),
           (im, ex, mp.replace(' entry point', ' 0001:00000030 ?foo@app@sco@@YAXXZ 0000000140001030 f core.obj\n entry point')),
           (im, ex, mp.replace(' entry point', ' 0001:00000040 ?CreateExecutableAuthority@reserved@sco@@YAXXZ 0000000140001040 f core.obj\n entry point'))]
    for args in bad:
        try:
            check(*args)
        except ValueError:
            continue
        raise AssertionError('negative parser fixture passed')
    print('PASS: synthetic parser fixtures only; no native Windows artifact tested')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--selftest', action='store_true')
    parser.add_argument('--dumpbin')
    parser.add_argument('--exe', type=pathlib.Path)
    parser.add_argument('--map', type=pathlib.Path)
    args = parser.parse_args()
    if args.selftest:
        selftest()
        return
    if not all((args.dumpbin, args.exe, args.map)):
        parser.error('--dumpbin, --exe and --map are required')
    def dump(flag):
        proc = subprocess.run([args.dumpbin, '/nologo', flag, str(args.exe)],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True)
        return proc.stdout.decode('utf-8', errors='replace')
    imports, exports = dump('/imports'), dump('/exports')
    try:
        dlls = check(imports, exports, args.map.read_text(encoding='utf-8', errors='replace'))
    except ValueError:
        print('--- dumpbin imports ---\n' + imports + '\n--- dumpbin exports ---\n' + exports, file=sys.stderr)
        raise
    print('PASS: public map authority and bounded symbol checks; no PE exports; reviewed PE dependencies:', dlls)
    print('Map symbols and PE imports do not establish complete code or runtime-loaded module closure.')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print('FAIL:', error, file=sys.stderr)
        sys.exit(1)
