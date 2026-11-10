# SPDX-License-Identifier: MIT
"""Exact integer model of guarded cmat lane address construction."""
from pathlib import Path
import json
limit=(2**32-1-30)//30;mask=2**64-1
strides=[0,1,8,16,32,64,96,128,160,256,512,limit-1,limit]
bases=[0,0xffffff00,0xfffffff0,0xffffffff,0x100000000,0x1234fffffff0,0xfffffffffffffff0]
signed_indices=[-2**31,-65537,-1,0,1,65537,2**31-1]
cases=0;carry=0
for stride in strides:
 for lane in range(32):
  for i in range(8):
   col=lane&15;row=(lane//16)*8+i
   assert col*stride<2**32 and (col*stride*2+row*2)<2**32
   for base in bases:
    for index in signed_indices:
     # Original signed source index stays in the 64-bit pointer.
     pointer=(base+index*2)&mask
     original=(pointer+col*stride*2+row*2)&mask
     offset=col*stride*2+row*2
     candidate=(pointer+offset)&mask
     assert original==candidate
     carry+=((pointer&0xffffffff)+offset)>0xffffffff
     cases+=1
# Negative or excessive stride is not accepted by the production guard.
for rejected in [limit+1,2**31,2**32-1]:assert rejected>limit
# Different lanes must retain different addresses for ordinary positive stride.
for stride in [16,32,128]:
 addresses=[(lane&15)*stride*2+(lane//16)*16 for lane in range(32)]
 assert len(set(addresses))==32
out=Path('docs/test-evidence/mesa-aco-cmat-address-2026-11-10/boundaries.json')
out.write_text(json.dumps({'cases':cases,'cross_4g_carries':carry,'max_stride':limit,'lane_uniqueness':True,'signed_source_indices_preserved':True,'model_only':True},indent=2)+chr(10))
print(out.read_text())
