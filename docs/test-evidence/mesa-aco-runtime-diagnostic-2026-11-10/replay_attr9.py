import struct,ctypes,json,math,re
from pathlib import Path
p=Path('build/aco-runtime-trace/runs-approved-2026-11-10/fast-trace-6/baseline-trace-6')
prm=(p/'3-d3d_enc0_attr_f_lt-parameters.bin').read_bytes()
def f(x):return struct.unpack('<f',struct.pack('<f',x))[0]
def C(x):return struct.unpack_from('<f',prm,x-0x160)[0]
def I(x):return struct.unpack_from('<i',prm,x-0x160)[0]
lib=ctypes.CDLL('libm.so.6')
for n,c in [('fmaf',3),('exp2f',1),('log2f',1)]:
 fn=getattr(lib,n);fn.argtypes=[ctypes.c_float]*c;fn.restype=ctypes.c_float
def fm(a,b,c):return lib.fmaf(a,b,c)
def half(x):return struct.unpack('<e',struct.pack('<e',x))[0]
def bits(x):return struct.unpack('<H',struct.pack('<e',x))[0]
mv=struct.unpack_from('<ee',(p/'input-guide-6.bin').read_bytes(),(378*1280+203)*4)
mv=[f(f(mv[i]+C(0x328+4*i))*C(0x320+4*i)) for i in range(2)]
ox=f(203.5-C(0x2f0));oy=f(378.5-C(0x2f4))
u=f(fm(ox,C(0x300),mv[0])*C(0x358));v=f(fm(oy,C(0x304),mv[1])*C(0x35c))
u2=f(f(u*f(I(0x350)))*C(0x360));v2=f(f(v*f(I(0x354)))*C(0x364))
text=(p/'trace.txt').read_text();pc=bytes.fromhex(re.search(r'dispatch 3-.* push=(.*)',text)[1])
sz=struct.unpack_from('<ii',pc,176)
x=f(f(u2*sz[0])-.5);y=f(f(v2*sz[1])-.5);ix=math.floor(x);iy=math.floor(y)
k=[round(f(f(x-ix)*256)),round(f(f(y-iy)*256))];fx=f(k[0]/256);fy=f(k[1]/256)
pool=(p/'1-pooled_history2_lt-pooled-history.bin').read_bytes()
taps=[];packed_taps=[]
for dx,dy in ((0,0),(1,0),(0,1),(1,1)):
 xx=min(max(ix+dx,0),sz[0]-1);yy=min(max(iy+dy,0),sz[1]-1)
 word=struct.unpack_from('<I',pool,(yy*sz[0]+xx)*4)[0]
 packed_taps.append(word)
 taps.append(f((word&1023)*f(1/1023)))
expo,prev=struct.unpack_from('<ff',(p/'2-d3d_ae-exposure-block.bin').read_bytes())
out={'xy':[203,378],'motion':mv,'uv2':[u2,v2],'size2':sz,'texel':[ix,iy],'fraction256':k,'taps':taps,'exposure':[expo,prev],'attr_scale':C(0x3a4),'evaluations':[]}
dll=Path('/mnt/c/Users/Kenny/Downloads/dlssnr_on_amd_setup/dist/weights/nvngx_dlssd.dll').read_bytes()
noise=struct.unpack_from('<H',dll,0x737350+2*(6*256+(378&15)*16+(203&15)))[0]>>9
case=Path('build/aco-runtime-trace/attr9/case.bin')
case.parent.mkdir(parents=True,exist_ok=True)
case.write_bytes(struct.pack('<6I3fI',*packed_taps,*k,expo,prev,C(0x3a4),noise))
def hf(b):return struct.unpack('<e',struct.pack('<H',b))[0]
out['noise']=noise
out['quantization_neighbours']=[]
for h in (0xb23b,0xb23c):
 m=h&0xfc00; t=half(hf(h)+hf(m|noise));r=half(t-hf(m))
 out['quantization_neighbours'].append({'input_bits':hex(h),'input':hf(h),'dithered_bits':hex(bits(r)),'dithered':r})
out['half_midpoint']=(hf(0xb23b)+hf(0xb23c))/2
for fused in (False,True):
 a,b,c,d=taps
 top=fm(a,f(1-fx),f(b*fx)) if fused else f(f(a*f(1-fx))+f(b*fx))
 bot=fm(c,f(1-fx),f(d*fx)) if fused else f(f(c*f(1-fx))+f(d*fx))
 h=fm(top,f(1-fy),f(bot*fy)) if fused else f(f(top*f(1-fy))+f(bot*fy))
 t=min(max(f(h*f(2.45)),f(1e-4)),3.)
 z=f(f(t*f(2.7777776718139648438))*f(3.3219280242919921875))
 dec=f(f(f(lib.exp2f(z)-1)*f(.0099999997764825820923))*f(1/prev))
 color=f(expo*dec);lg=f(f(lib.log2f(max(fm(color,100.,1.),f(1e-4)))*f(.30103000998497009277))*f(.36000001430511474609))
 val=f(lg*C(0x3a4))
 out['evaluations'].append({'bilinear_fused':fused,'history':h,'decoded':dec,'attribute_f32':val,'attribute_half':bits(val),'attribute_value':half(val)})
print(json.dumps(out,indent=2))
out['blend_contraction_enumeration']=[]
def blend_add(a,b,c,d,mode):
 return f(f(a*b)+f(c*d)) if mode==0 else fm(a,b,f(c*d)) if mode==1 else fm(c,d,f(a*b))
for top_mode in range(3):
 for bottom_mode in range(3):
  for final_mode in range(3):
   top=blend_add(taps[0],f(1-fx),taps[1],fx,top_mode)
   bot=blend_add(taps[2],f(1-fx),taps[3],fx,bottom_mode)
   h=blend_add(top,f(1-fy),bot,fy,final_mode)
   out['blend_contraction_enumeration'].append(dict(modes=[top_mode,bottom_mode,final_mode],value=h,bits=hex(struct.unpack('<I',struct.pack('<f',h))[0])))
top=fm(f(taps[1]-taps[0]),fx,taps[0]);bot=fm(f(taps[3]-taps[2]),fx,taps[2]);h=fm(f(bot-top),fy,top)
out['lerp_form']=dict(value=h,bits=hex(struct.unpack('<I',struct.pack('<f',h))[0]))
out['normalization_after_blend']=[]
for tm in range(3):
 for bm in range(3):
  for ym in range(3):
   t=[float(w&1023) for w in packed_taps]
   top=blend_add(t[0],f(1-fx),t[1],fx,tm);bot=blend_add(t[2],f(1-fx),t[3],fx,bm)
   h=f(blend_add(top,f(1-fy),bot,fy,ym)*f(1/1023))
   out['normalization_after_blend'].append(dict(modes=[tm,bm,ym],value=h,bits=hex(struct.unpack('<I',struct.pack('<f',h))[0])))
print('normalization-after-blend values',sorted(set(x['bits'] for x in out['normalization_after_blend'])))
weight1=f(k[0]*f(1/(1023*256)));weight0=fm(-f(1/(1023*256)),k[0],f(1/1023))
t=[float(w&1023) for w in packed_taps]
top=f(f(t[0]*weight0)+f(t[1]*weight1));bot=f(f(t[2]*weight0)+f(t[3]*weight1))
h=f(f(top*f(1-fy))+f(bot*fy))
out['llpc_isa_weights_first']=dict(weight0=weight0,weight1=weight1,top=top,bottom=bot,value=h,bits=hex(struct.unpack('<I',struct.pack('<f',h))[0]))
print('LLPC ISA weights-first',out['llpc_isa_weights_first'])
print('possible blend values',sorted(set(x['bits'] for x in out['blend_contraction_enumeration'])), 'lerp',out['lerp_form'])
(p.parent.parent/'attr9-cpu-replay.json').write_text(json.dumps(out,indent=2)+'\n')
