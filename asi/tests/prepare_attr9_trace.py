#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate an isolated full-kernel probe; never modify staged shaders/records."""
from pathlib import Path
import shutil
src=Path('gpu/shaders/enc0_attr.comp').read_text()
out=Path('build/aco-runtime-trace/attr9-full')
(out/'source').mkdir(parents=True,exist_ok=True)
(out/'data/shaders').mkdir(parents=True,exist_ok=True)
for p in Path('build/asi/stage/openrr-amd/shaders').glob('*.spv'):
    shutil.copy2(p,out/'data/shaders'/p.name)
def replace(a,b):
    global src
    assert src.count(a)==1,(a,src.count(a))
    src=src.replace(a,b)
replace('} pc;', '''} pc;
// Probe storage is after all padded ATTR64 records, before allocation end in
// this 1280x720 case. Later kernels may reuse it; copy immediately after attr.
bool probe9 = false;
void dbg9(uint slot,uint value) {
    if(gl_GlobalInvocationID.x==484043u && pc.frame8==6u && pc.size==ivec2(1280,720))
        U32Buf(uint64_t(pc.attr)+uint64_t(pc.pad.x*pc.pad.y)*48ul).v[slot]=value;
}
void dbg9f(uint slot,float value){dbg9(slot,floatBitsToUint(value));}''')
replace('    BLEND4(T10, 3)\n    return r;', '''    BLEND4(T10, 3)
    if(probe9) {
        dbg9f(0u,u);dbg9f(1u,v);dbg9(2u,uint(p.x));dbg9(3u,uint(p.y));
        dbg9(4u,k.x);dbg9(5u,k.y);dbg9f(6u,r.x);
        dbg9f(7u,tex_10(b,sz,p).x);dbg9f(8u,tex_10(b,sz,p+ivec2(1,0)).x);
        dbg9f(9u,tex_10(b,sz,p+ivec2(0,1)).x);dbg9f(10u,tex_10(b,sz,p+ivec2(1,1)).x);
    }
    return r;''')
replace('    return r;\n}\n\n#ifdef LT_ATTR_CL', '''    if(probe9){dbg9f(11u,v);dbg9f(12u,rexp);dbg9f(13u,t);dbg9f(14u,a);dbg9f(15u,b);dbg9f(16u,e);dbg9f(17u,r);}
    return r;
}

#ifdef LT_ATTR_CL''')
replace('    return r;\n}\n\nbool inside', '''    if(probe9){dbg9f(18u,x);dbg9f(19u,a);dbg9f(20u,r);}
    return r;
}

bool inside''')
replace('            vec4 h2 = bl_10(pc.ds2, pc.ds2_size, u2, v2);', '''            probe9 = true;
            vec4 h2 = bl_10(pc.ds2, pc.ds2_size, u2, v2);
            probe9 = false;''')
replace('                float y = valid ? decode_hist(h2[c], rexp) : col[c];', '''                probe9 = c==0u;
                float y = valid ? decode_hist(h2[c], rexp) : col[c];''')
replace('                put(base, 9u + c, min(no_nan(e), 1024.0));\n            }\n#endif', '''                put(base, 9u + c, min(no_nan(e), 1024.0));
                if(probe9){dbg9f(21u,e);dbg9(22u,uint(float16BitsToUint16(A[9])));dbg9(23u,valid?1u:0u);}
                probe9 = false;
            }
#endif''')
replace('        uint d = packFloat2x16(r);', '''        uint d = packFloat2x16(r);
        if(q==4u){dbg9(24u,w);dbg9(25u,m);dbg9(26u,nn);dbg9(27u,packFloat2x16(tt));dbg9(28u,d);}''')
(out/'source/enc0_attr.comp').write_text(src)
print(out)
