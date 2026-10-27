#!/usr/bin/env python3
"""Verify/regenerate the build-gated Skyrim unlock table from an external NSO.

Usage: check_skyrim_unlocks.py <original main> [--write <unlock_release.tsv>]
"""
from pathlib import Path
import sys, struct
sys.path.insert(0,str(Path(__file__).resolve().parent))
from nso_syms import load
from capstone import Cs,CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN
from capstone.arm64 import ARM64_OP_IMM
import argparse
p=argparse.ArgumentParser(description='Verify and regenerate Skyrim base unlock-order cuts')
p.add_argument('main',type=Path);p.add_argument('--write',type=Path);args=p.parse_args()
raw=args.main.read_bytes()
if raw[0x40:0x50].hex()!='b3f6e5da5c501857d754e3e186e04dc2':p.error('unsupported main build ID')
d=load(args.main)
cs=Cs(CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN);cs.detail=True
end=0x112f7b4
cache={}
def ins(a):
 if a not in cache:cache[a]=next(cs.disasm(d[a:a+4],a),None)
 return cache[a]
def dead_after(a,reg):
 todo=[a];seen=set()
 while todo:
  pc=todo.pop()
  if pc in seen:continue
  seen.add(pc)
  if not 0<=pc<end or len(seen)>4096:return False
  i=ins(pc)
  if not i:return False
  reads,writes=i.regs_access()
  def canon(r, instruction=i):
   name=instruction.reg_name(r)
   return name.replace('w','x',1) if name.startswith('w') else name
  if canon(reg) in {canon(r) for r in reads}:return False
  if canon(reg) in {canon(r) for r in writes}:continue
  if i.mnemonic in ('ret','br'):return False
  if i.mnemonic=='b':todo.append(i.operands[0].imm)
  elif i.mnemonic.startswith('b.') or i.mnemonic in ('cbz','cbnz','tbz','tbnz'):
   todo.extend([pc+4,i.operands[-1].imm])
  else:todo.append(pc+4)
 return True
rows=[];errors=[]
for a in range(0,end,4):
 w=struct.unpack_from('<I',d,a)[0]
 if w&0xfc000000!=0x94000000:continue
 im=w&0x3ffffff
 if im&0x2000000:im-=0x4000000
 if a+im*4!=0x112d9f8 or struct.unpack_from('<I',d,a+4)[0]!=0xd5033bbf:continue
 prev=[ins(x) for x in range(a-12,a,4)]
 stores=[i for i in prev if i.mnemonic=='str' and i.op_str.startswith('wzr,')]
 if not stores:continue
 i=stores[-1];r=i.operands[1].mem.base;rn=int(i.reg_name(r)[1:]);off=i.operands[1].mem.disp
 if i.writeback:
  nxt=ins(i.address+4)
  ok=(i.address==a-8 and i.operands[-1].type==ARM64_OP_IMM and i.operands[-1].imm==4 and nxt.mnemonic=='mov' and nxt.op_str==f'x0, x{rn}' and dead_after(a+4,r))
  if ok:
   rows.append((i.address,struct.unpack_from('<I',d,i.address)[0],0x889ffc00|(rn<<5)|31,'post-index release'))
   rows.append((i.address+4,struct.unpack_from('<I',d,i.address+4)[0],0x91001000|(rn<<5),'event address after release'))
  else:errors.append((hex(i.address),i.op_str,'post-index proof failed'))
 elif off==0:
  rows.append((i.address,struct.unpack_from('<I',d,i.address)[0],0x889ffc00|(rn<<5)|31,'release'))
 elif -256<=off<=255:
  rows.append((i.address,struct.unpack_from('<I',d,i.address)[0],0x99000000|((off&511)<<12)|(rn<<5)|31,'unscaled release'))
 else:
  setup=ins(i.address-4)
  if setup.mnemonic=='add' and setup.op_str==f'x0, x{rn}, #{off+4:#x}':
   rows.append((i.address,struct.unpack_from('<I',d,i.address)[0],0x991fc01f,'release through event-4'))
  else:errors.append((hex(i.address),i.op_str,'large offset proof failed',setup.op_str))
# Three post-indexed x0 cases need to preserve x0's update. A tiny tail
# wrapper in verified text padding releases the original lock, advances x0,
# then tail-calls SignalLightEvent. The original call retains its LR and DMB.
stub=0x112f7c0
assert end<=stub and stub+12<=struct.unpack_from('<I',raw,0x24)[0]
assert d[stub:stub+12]==bytes(12)
def branch(at,target,link=False):
 delta=target-at
 assert delta%4==0 and -(1<<27)<=delta<(1<<27)
 return (0x94000000 if link else 0x14000000)|((delta//4)&0x3ffffff)
assert {int(e[0],16) for e in errors}=={0x1ab760,0xa56e3c,0xb1539c}, errors
for e in errors:
 at=int(e[0],16)
 assert ins(at).op_str=='wzr, [x0], #4' and ins(at+4).mnemonic=='bl'
 rows.extend([(at,struct.unpack_from('<I',d,at)[0],0xd503201f,'unlock moved to release wrapper'),
              (at+4,struct.unpack_from('<I',d,at+4)[0],branch(at+4,stub,True),'call release wrapper')])
blob=struct.pack('<III',0x889ffc1f,0x91001000,branch(stub+8,0x112d9f8))
assert len({x[0] for x in rows})==len(rows)
# The pointer base must not be Apple's reserved x18 in an instruction the
# general rewriter does not decode (STLUR). No accepted site uses it.
assert not any(((new>>5)&31)==18 for _,_,new,kind in rows if 'release' in kind and new&0xff000000==0x99000000)
lines=['# Skyrim main b3f6e5da5c501857d754e3e186e04dc2; check_skyrim_unlocks.py',
       '# offset\toriginal bytes\treplacement bytes\treason']
for at,old,new,why in sorted(rows):
 lines.append(f'{at:#x}\t{struct.pack("<I",old).hex()}\t{struct.pack("<I",new).hex()}\t{why}')
lines.append(f'{stub:#x}\t{bytes(12).hex()}\t{blob.hex()}\trelease wrapper in verified executable text padding')
text='\n'.join(lines)+'\n'
if args.write:args.write.write_text(text)
else:
 table=Path(__file__).resolve().parents[1]/'games/01000A10041EA000/unlock_release.tsv'
 assert table.read_text()==text, 'unlock table is stale; regenerate with --write'
print(f'{len(rows)+1} cuts covering 517 plain-store SignalLightEvent sites; all bytes and liveness checks passed')
