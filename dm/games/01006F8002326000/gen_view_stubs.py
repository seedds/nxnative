"""Assemble an owned ARM64 adapter and retain its symbolic relocations.

Usage: gen_view_stubs.py source.s generated.py. Requires Apple's clang only
when changing the assembly; normal NSO rewriting consumes the generated data.
"""
import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile


def assemble(source):
    with tempfile.TemporaryDirectory() as folder:
        out=Path(folder)/'stubs.o'
        subprocess.run(['clang','-arch','arm64','-c',str(source),'-o',str(out)],check=True)
        blob=out.read_bytes()
    assert struct.unpack_from('<I',blob)[0]==0xfeedfacf
    cursor=32;section=None;symbols=None
    for _ in range(struct.unpack_from('<I',blob,16)[0]):
        cmd,size=struct.unpack_from('<II',blob,cursor)
        if cmd==0x19:
            for j in range(struct.unpack_from('<I',blob,cursor+64)[0]):
                at=cursor+72+80*j
                if blob[at:at+16].rstrip(b'\0')==b'__text':
                    section=(struct.unpack_from('<Q',blob,at+40)[0],*struct.unpack_from('<IIII',blob,at+48))
        if cmd==2:symbols=struct.unpack_from('<IIII',blob,cursor+8)
        cursor+=size
    assert section and symbols
    length,offset,align,reloff,nreloc=section
    symoff,count,stroff,strsize=symbols;strings=blob[stroff:stroff+strsize];entries=[]
    for j in range(count):
        name,kind,sect,desc,value=struct.unpack_from('<IBBHQ',blob,symoff+16*j)
        entries.append((strings[name:strings.index(b'\0',name)].decode(),kind,sect,value))
    names={name:value for name,kind,sect,value in entries if kind&0xe==0xe and sect==1 and kind&1}
    relocs=[]
    for j in range(nreloc):
        address,info=struct.unpack_from('<II',blob,reloff+8*j)
        symbol=info&0xffffff;pcrel=(info>>24)&1;scale=(info>>25)&3;external=(info>>27)&1;kind=info>>28
        assert external and scale==2 and kind in (2,3,4), (address,hex(info))
        assert pcrel==(kind in (2,3))
        relocs.append((address,kind,entries[symbol][0]))
    code=blob[offset:offset+length];assert len(code)%4==0
    return list(struct.unpack('<'+'I'*(len(code)//4),code)),names,sorted(relocs)


def generate(source):
    words,symbols,relocs=assemble(source)
    lines=['# Generated from '+source.name+' by dm/tools/gen_view_stubs.py; do not edit.',
           'source_sha256 = '+repr(hashlib.sha256(source.read_bytes()).hexdigest()),'words = [']
    lines += ['    '+', '.join(f'0x{v:08x}' for v in words[i:i+6])+',' for i in range(0,len(words),6)]
    lines += [']','symbols = '+repr(dict(sorted(symbols.items()))),'relocations = '+repr(relocs),'']
    return '\n'.join(lines)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('source',type=Path);parser.add_argument('output',type=Path);args=parser.parse_args()
    generated=generate(args.source)
    if args.output.exists() and '# BEGIN GENERATED VIEW CODE' in args.output.read_text():
        old=args.output.read_text();start=old.index('# BEGIN GENERATED VIEW CODE');end=old.index('# END GENERATED VIEW CODE',start)
        args.output.write_text(old[:start]+'# BEGIN GENERATED VIEW CODE\n'+generated+old[end:])
    else:args.output.write_text(generated)
