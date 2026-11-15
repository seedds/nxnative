"""Exercise the host's actual pending-write functions without a Metal/game build."""
from pathlib import Path
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1] / 'nvn/nvn_mtl.m'


def function(source, name):
    import re
    match = re.search(r'^static (?:inline )?[^\n]+\b' + name + r'\([^\n]*\) \{', source, re.M)
    if not match: raise ValueError(name)
    begin = source.index('{', match.start()); depth = 1; end = begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}'); end += 1
    return source[match.start():end]


class PendingWide(unittest.TestCase):
    def test_wide_growth_latest_writes_reset_and_scratch(self):
        source = SOURCE.read_text()
        # Use production declarations; only the GPU update-buffer entry is opaque
        # here, since these functions merely reserve its array.
        declarations = source[source.index('enum { PEND_HB_BITS'):source.index('static int pend_grow')]
        a = declarations.index('typedef struct { uint64_t lo; size_t off;')
        b = declarations.index('static upd_ent *g_upd;', a)
        declarations = declarations[:a] + 'typedef struct { int unused; } upd_ent;\n' + declarations[b:]
        code = '#include <stdint.h>\n#include <stddef.h>\n#include <stdlib.h>\n#include <limits.h>\n#include <assert.h>\n#include <stdio.h>\n#include <string.h>\n'
        code += declarations + '\n'
        code += 'static int g_nhw_sub,g_nsbw,g_nsbw_w,g_sbw_over; static uint64_t g_sbwp[1];\n'
        code += 'static void vc_gpu_write(uint64_t a,uint64_t b) {}\nstatic void drw_note(uint64_t a,uint64_t b) {}\n'
        for name in ('pend_grow', 'pend_room', 'pend_bucket', 'pend_reset', 'pend_add', 'pend_last', 'pend_hits', 'pend_since'):
            code += function(source, name) + '\n'
        code += r'''
int main(void) {
 const uint64_t base=0x10000000, step=0x20000;
 // Exercise several entry reallocations, beyond the old 256-wide-write cap.
 for(int i=0;i<9000;i++) {
  int e=pend_add(base+step*i,0x10000,1);
  if(e!=i) fprintf(stderr,"tracking stopped at wide write %d (overflow=%d)\n",i+1,g_pend_over);
  assert(e==i);
 }
 assert(!g_pend_over && g_nwide==9000 && g_npend==9000);
 assert(!pend_hits(0x1000,4));
 for(int i=0;i<9000;i++) assert(pend_last(base+step*i+8,4)==i);
 // A later small handle update overrides the large write covering it.
 uint64_t handle=base+step*8999+8;
 int small=pend_add(handle,4,1);
 assert(pend_last(handle,4)==small);
 assert(pend_last(handle+8,4)==8999);
 // A mixed lookup must retain its newest-first order and remove duplicates.
 int n=pend_since(handle,handle+4,0,g_ubp_idx);
 assert(n==2 && g_ubp_idx[0]==small && g_ubp_idx[1]==8999);
 // All overlapping wide writes plus narrow hits must fit in scratch storage.
 for(int i=0;i<10000;i++) assert(pend_add(handle-0x100,0x10000,1)>=0);
 n=pend_since(handle,handle+4,0,g_ubp_idx);
 assert(n==10002 && g_ubp_idx[0]==g_npend-1);
 for(int i=1;i<n;i++) assert(g_ubp_idx[i-1]>g_ubp_idx[i]);
 // Reset only counters/generation as pend_reset does, preserving allocations.
 int cap=g_pend_cap;pend_reset();
 assert(pend_last(handle,4)==-1);
 assert(pend_add(base,0x10000,2)==0 && g_pend_cap==cap);
 assert(pend_last(base+8,4)==0 && !pend_hits(handle,4));
 g_pend_gen=UINT32_MAX;pend_reset();
 assert(g_pend_gen==1 && pend_last(base+8,4)==-1);
 puts("pending wide writes: growth, latest writer, scratch bounds and reset passed");
 free(g_pend_lo);free(g_pend_hi);free(g_pend_kind);free(g_pend_upd);
 free(g_upd);free(g_pend_wide);free(g_pend_node);free(g_ubp_idx);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='pend-wide-') as temp:
            root=Path(temp);(root/'test.c').write_text(code)
            result = subprocess.run(['clang','-O1','-fsanitize=address,undefined',str(root/'test.c'),'-o',str(root/'test')],capture_output=True,text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(root/'test')],check=True)


if __name__ == '__main__': unittest.main()
