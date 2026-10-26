#!/usr/bin/env python3
"""Create a tvOS brand-asset catalog from the existing iOS icon artwork."""
import argparse
import json
from pathlib import Path
import subprocess

INFO = {"author": "xcode", "version": 1}


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n")


def create(source, output):
    helper = Path(__file__).with_suffix('.swift')
    renderer = output.parent / 'tv-icon-renderer'
    subprocess.run(['swiftc', '-module-cache-path', str(output.parent / 'icon-module-cache'), str(helper), '-o', str(renderer)], check=True)
    output.mkdir(parents=True, exist_ok=True)
    write(output / 'Contents.json', {"info": INFO})
    brand = output / 'App Icon & Top Shelf Image.brandassets'
    assets = []
    for name, width, height, role, scale in (
        ('App Icon', 400, 240, 'primary-app-icon', True),
        ('App Icon - App Store', 1280, 768, 'primary-app-icon', False),
        ('Top Shelf', 1920, 720, 'top-shelf-image', True),
        ('Top Shelf Wide', 2320, 720, 'top-shelf-image-wide', True)):
        stack = role == 'primary-app-icon'
        filename = name + ('.imagestack' if stack else '.imageset')
        asset = brand / filename
        assets.append({'idiom': 'tv', 'size': f'{width}x{height}', 'role': role, 'filename': filename})
        layers = ['Front', 'Back'] if stack else ['Full']
        if stack:
            write(asset / 'Contents.json', {'info': INFO, 'layers': [{'filename': x + '.imagestacklayer'} for x in layers]})
        for layer in layers:
            imageset = asset / (layer + '.imagestacklayer') / 'Content.imageset' if stack else asset
            if stack:
                write(imageset.parent / 'Contents.json', {'info': INFO})
            images = []
            for multiplier in ([1, 2] if scale else [1]):
                image_name = f'{layer.lower()}-{multiplier}x.png'
                images.append({'idiom': 'tv', 'scale': f'{multiplier}x', 'filename': image_name})
                imageset.mkdir(parents=True, exist_ok=True)
                subprocess.run([str(renderer), str(source), str(imageset / image_name),
                    str(width * multiplier), str(height * multiplier), layer.lower()], check=True)
            write(imageset / 'Contents.json', {'info': INFO, 'images': images})
    write(brand / 'Contents.json', {'info': INFO, 'assets': assets})
    renderer.unlink()


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source', type=Path)
    p.add_argument('output', type=Path)
    a = p.parse_args()
    create(a.source.resolve(), a.output.resolve())
