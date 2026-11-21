#!/usr/bin/env python3
"""Regenerate the synthetic NX corpus. Rust is needed only for this command."""
from pathlib import Path
import subprocess
import tempfile

REVISION = 'd40e62d978ee2cd1a1261415f59b5aa5302ccfe6'
HERE = Path(__file__).resolve().parent


def main():
    with tempfile.TemporaryDirectory(prefix='nxnative-fixtures-') as tmp:
        root = Path(tmp); source = root / 'nxformat'
        subprocess.run(['git', 'clone', '--filter=blob:none', '--no-checkout', 'https://github.com/fab918/nxformat.git', str(source)], check=True)
        subprocess.run(['git', '-C', str(source), 'checkout', REVISION, '--', 'packer'], check=True)
        # The upstream inventory fixtures scan directory metadata directly. Give
        # their RomFS hash buckets/root name and Patch CNMT tail the complete
        # layouts LibHac needs for path lookup and extended-metadata parsing.
        builder = source / 'packer/src/native/synthetic.rs'
        text = builder.read_text()
        edits = {
            'put_u32(&mut e, 0x18, u32::MAX);': 'put_u32(&mut e, 0x18, next);',
            'put_u32(&mut dir_meta, 20, u32::MAX);': 'put_u32(&mut dir_meta, 20, 0);',
            'let hash_table = u32::MAX.to_le_bytes().to_vec();': 'let dir_hash_table = 0u32.to_le_bytes().to_vec();\n    let file_hash_table = if files.is_empty() {u32::MAX} else {0}.to_le_bytes().to_vec();',
            '0x08, &hash_table': '0x08, &dir_hash_table',
            '0x28, &hash_table': '0x28, &file_hash_table',
            '    let cnmt_name = format!(': '    if spec.meta_type == 0x81 {\n        put_u32(&mut cnmt, 0x2c, 0x1c);\n        cnmt.extend_from_slice(&[0; 0x1c]);\n    }\n    let digest = sha256(&cnmt);\n    cnmt.extend_from_slice(&digest);\n    let cnmt_name = format!(',
        }
        for before, after in edits.items():
            if text.count(before) != 1: raise RuntimeError(f'upstream fixture builder changed: {before}')
            text = text.replace(before, after)
        builder.write_text(text)
        (root / 'src').mkdir(); (root / 'src/main.rs').write_bytes((HERE / 'generate.rs').read_bytes())
        (root / 'Cargo.toml').write_text('[package]\nname="nxnative-fixtures"\nversion="0.0.0"\nedition="2021"\n[dependencies]\nnxformat-packer={path="nxformat/packer"}\n')
        subprocess.run(['cargo', 'run', '--release', '--manifest-path', str(root / 'Cargo.toml'), '--', str(HERE)], check=True)


if __name__ == '__main__': main()
