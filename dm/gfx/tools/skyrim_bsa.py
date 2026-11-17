#!/usr/bin/env python3
"""Extract FXP shader files from Skyrim Switch's version-105 shader BSA."""
import argparse
import os
from pathlib import PurePosixPath
import struct
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../../tools'))
from romfs import open as romfs_open

SKYRIM_TITLE_ID = '01000A10041EA000'
SHADER_ARCHIVE = 'skyrim - shaders.bsa'
HEADER = struct.Struct('<4s8I')
FOLDER_RECORD = struct.Struct('<QIIQ')
FILE_RECORD = struct.Struct('<QII')
COMPRESSED = 0x40000000
SIZE_MASK = 0x3fffffff


def _read_exact(f, size, where):
    data = f.read(size)
    if len(data) != size:
        raise ValueError(f'{where}: expected {size:#x} bytes, got {len(data):#x}')
    return data


def _member_path(folder, name, archive):
    folder = os.fsdecode(folder).replace('\\', '/')
    name = os.fsdecode(name).replace('\\', '/')
    combined = '/'.join(part for part in (folder, name) if part)
    path = PurePosixPath(combined)
    if '\0' in combined or path.is_absolute() or any(part in ('', '.', '..') for part in combined.split('/')):
        raise ValueError(f'{archive}: unsafe BSA member path {combined!r}')
    return path


def _copy_member(src, dst, offset, archive_flags, size_flags, label):
    if size_flags & 0x80000000:
        raise ValueError(f'{label}: unsupported BSA file-size flag {size_flags:#x}')
    size = size_flags & SIZE_MASK
    compressed = bool(archive_flags & 0x4) ^ bool(size_flags & COMPRESSED)
    if compressed:
        raise ValueError(f'{label}: compressed shader members are not supported')
    src.seek(offset)
    remaining = size
    while remaining:
        block = src.read(min(1 << 20, remaining))
        if not block:
            raise ValueError(f'{label}: member data ends before its declared size')
        dst.write(block)
        remaining -= len(block)


def extract_shaders(archive, output_dir):
    """Extract only .fxp members from Skyrim Switch's version-105 shader BSA."""
    if os.path.basename(os.fspath(archive)).lower() != SHADER_ARCHIVE:
        raise ValueError(f'{archive}: expected {SHADER_ARCHIVE}')

    with romfs_open(archive, 'rb') as src:
        src.seek(0, os.SEEK_END)
        archive_size = src.tell()
        src.seek(0)
        header = _read_exact(src, HEADER.size, str(archive))
        magic, version, folder_offset, archive_flags, folder_count, file_count, folder_names_size, file_names_size, _ = HEADER.unpack(header)
        if magic != b'BSA\0' or version != 105:
            raise ValueError(f'{archive}: expected BSA version 105, got {magic!r} version {version}')
        if folder_offset < HEADER.size or folder_offset + folder_count * FOLDER_RECORD.size > archive_size:
            raise ValueError(f'{archive}: folder table lies outside the archive')
        if not archive_flags & 1 or not archive_flags & 2:
            raise ValueError(f'{archive}: folder and file names are required')
        if (folder_count > archive_size // FOLDER_RECORD.size or file_count > archive_size // FILE_RECORD.size
                or folder_names_size > archive_size or file_names_size > archive_size):
            raise ValueError(f'{archive}: unreasonable BSA entry counts')

        src.seek(folder_offset)
        folders = []
        total_files = 0
        for _ in range(folder_count):
            _, count, _, _ = FOLDER_RECORD.unpack(_read_exact(src, FOLDER_RECORD.size, str(archive)))
            folders.append(count)
            total_files += count
        if total_files != file_count:
            raise ValueError(f'{archive}: folder records list {total_files} files, header says {file_count}')

        records = []
        folder_names_read = 0
        for count in folders:
            name_len = _read_exact(src, 1, str(archive))[0]
            folder_bytes = _read_exact(src, name_len, str(archive))
            folder_names_read += name_len
            if not name_len or not folder_bytes.endswith(b'\0'):
                raise ValueError(f'{archive}: invalid BSA folder name')
            folder = folder_bytes[:-1]
            for _ in range(count):
                _, size_flags, offset = FILE_RECORD.unpack(_read_exact(src, FILE_RECORD.size, str(archive)))
                size = size_flags & SIZE_MASK
                if offset > archive_size or size > archive_size - offset:
                    raise ValueError(f'{archive}: member at {offset:#x} of {size:#x} bytes lies outside the archive')
                records.append((folder, size_flags, offset))
        if folder_names_read != folder_names_size:
            raise ValueError(f'{archive}: folder names occupy {folder_names_read} bytes, header says {folder_names_size}')

        names_data = _read_exact(src, file_names_size, str(archive))
        data_offset = src.tell()
        names = names_data.split(b'\0')
        if names[-1] != b'' or len(names) != file_count + 1:
            raise ValueError(f'{archive}: file-name table does not match {file_count} entries')

        root = os.path.abspath(os.fspath(output_dir))
        extracted = []
        for (folder, size_flags, offset), name in zip(records, names):
            if not name.lower().endswith(b'.fxp'):
                continue
            if offset < data_offset:
                raise ValueError(f'{archive}: member data at {offset:#x} overlaps the BSA index')
            relative = _member_path(folder, name, archive)
            destination = os.path.abspath(os.path.join(root, *relative.parts))
            if os.path.commonpath((root, destination)) != root:
                raise ValueError(f'{archive}: member path escapes the output directory')
            os.makedirs(os.path.dirname(destination), exist_ok=True)
            fd, temporary = tempfile.mkstemp(prefix='.skyrim-', suffix='.tmp', dir=os.path.dirname(destination))
            try:
                with os.fdopen(fd, 'wb') as dst:
                    _copy_member(src, dst, offset, archive_flags, size_flags, f'{archive}/{relative.as_posix()}')
                os.replace(temporary, destination)
            finally:
                if os.path.exists(temporary):
                    os.unlink(temporary)
            extracted.append((destination, relative.as_posix()))

    if not extracted:
        raise ValueError(f'{archive}: no .fxp shader members found')
    return extracted


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', help='Skyrim - Shaders.bsa from the game RomFS')
    parser.add_argument('output_dir', help='directory for the extracted .fxp members')
    args = parser.parse_args(argv)
    if os.environ.get('DM_GAME', '').upper() != SKYRIM_TITLE_ID:
        parser.error(f'DM_GAME must be {SKYRIM_TITLE_ID}')
    try:
        extracted = extract_shaders(args.archive, args.output_dir)
    except (OSError, ValueError) as e:
        parser.error(str(e))
    print(f'extracted {len(extracted)} FXP file(s) to {os.path.abspath(args.output_dir)}')


if __name__ == '__main__':
    main()
