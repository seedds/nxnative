"""Shared platform selection for the Vulkan and OpenGL offline shader tools."""
import argparse
import os


def arguments(argv, inputs):
    ap = argparse.ArgumentParser(description=f'Translate {inputs} into Metal libraries')
    ap.add_argument('roots', nargs='+', help=inputs)
    ap.add_argument('out', help='Mac output root')
    ap.add_argument('--ios', help='iPhone output root')
    ap.add_argument('--targets', help='mac, ios, or mac,ios (default: both with --ios, otherwise mac)')
    args = ap.parse_args(argv)
    selected = args.targets if args.targets is not None else 'mac,ios' if args.ios else 'mac'
    args.targets = {t.strip() for t in selected.split(',')}
    if not args.targets or not args.targets <= {'mac', 'ios'}: ap.error('--targets takes mac, ios, or mac,ios')
    if 'ios' in args.targets and not args.ios: ap.error('--targets ios requires --ios <output root>')
    return args


def destinations(args, api):
    dirs = []
    if 'mac' in args.targets: dirs.append((os.path.join(args.out, api), 'macosx', []))
    # Relaxed math preserves the NaN/Inf handling in translated shaders, as in ios_metal.py.
    if 'ios' in args.targets: dirs.append((os.path.join(args.ios, api), 'iphoneos', ['-mios-version-min=18.0', '-fmetal-math-mode=relaxed']))
    return dirs
