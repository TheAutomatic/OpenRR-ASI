#pragma once
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdint>
namespace optifg_probe {
static int32_t delta(const unsigned char* p) { int32_t v; std::memcpy(&v,p,4); return v; }
// Match instruction roles, not compiler-selected stack/State displacements.
inline bool validate_block(const unsigned char* p, size_t beforeCall, size_t afterCall) {
 const unsigned char shape[]={0x80,0xbd,0,0,0,0,0,0x0f,0x84,0,0,0,0,
  0x80,0xbf,0,0,0,0,0,0x0f,0x84,0,0,0,0,
  0x4c,0x8d,0x85,0,0,0,0,0x48,0x8d,0x54,0x24,0,0x48,0x8d,0x4c,0x24,0};
 const char mask[]="xx????xxx????xx????xxx????xxx????xxxx?xxxx?";
 static_assert(sizeof(mask)-1==sizeof(shape));
 if(beforeCall<43 || afterCall<5) return false;
 for(size_t i=0;i<sizeof(shape);++i) if(mask[i]=='x' && p[i]!=shape[i]) return false;
 const int64_t a=13LL+delta(p+9), b=26LL+delta(p+22);
 // Both failures must reach the same earlier block inside this function.
 return a==b && a<0 && a>=43-static_cast<int64_t>(beforeCall);
}
inline unsigned char* locate(HMODULE m) {
 auto b=reinterpret_cast<unsigned char*>(m);
 auto d=reinterpret_cast<IMAGE_DOS_HEADER*>(b);
 auto n=reinterpret_cast<IMAGE_NT_HEADERS64*>(b+d->e_lfanew);
 auto sec=IMAGE_FIRST_SECTION(n);
 std::vector<unsigned char*> hits,refs,calls;
 const char anchor[]="DLSSDFeatureDx12::DLSSDFeatureDx12 binding complete!";
 for(unsigned s=0;s<n->FileHeader.NumberOfSections;s++) {
  if(sec[s].Characteristics&IMAGE_SCN_MEM_EXECUTE) continue;
  auto p=b+sec[s].VirtualAddress; auto len=sec[s].Misc.VirtualSize;
  for(size_t i=0;i+sizeof(anchor)<=len;i++) if(!std::memcmp(p+i,anchor,sizeof(anchor))) hits.push_back(p+i);
 }
 if(hits.size()==1) for(unsigned s=0;s<n->FileHeader.NumberOfSections;s++) {
  if(!(sec[s].Characteristics&IMAGE_SCN_MEM_EXECUTE)) continue;
  auto p=b+sec[s].VirtualAddress; auto len=sec[s].Misc.VirtualSize;
  for(size_t i=0;i+7<=len;i++) if((p[i]==0x48||p[i]==0x4c)&&p[i+1]==0x8d&&(p[i+2]&0xc7)==5&&p+i+7+delta(p+i+3)==hits[0]) refs.push_back(p+i);
 }
 DWORD64 image=0;
 auto factory=refs.size()==1 ? RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(refs[0]),&image,nullptr) : nullptr;
 if(factory && image==reinterpret_cast<DWORD64>(b) && !(b[factory->UnwindData]&0x20)) for(unsigned s=0;s<n->FileHeader.NumberOfSections;s++) {
  if(!(sec[s].Characteristics&IMAGE_SCN_MEM_EXECUTE)) continue;
  auto p=b+sec[s].VirtualAddress; auto len=sec[s].Misc.VirtualSize;
  for(size_t i=0;i+5<=len;i++) if(p[i]==0xe8&&p+i+5+delta(p+i+1)==b+factory->BeginAddress) calls.push_back(p+i);
 }
 auto f=calls.size()==1 ? RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(calls[0]),&image,nullptr) : nullptr;
 bool ok=f && image==reinterpret_cast<DWORD64>(b) && calls[0]>=b+f->BeginAddress+43;
 if(ok) {
  auto block=calls[0]-43;
  ok=validate_block(block, calls[0]-(b+f->BeginAddress), b+f->EndAddress-calls[0]);
  if(ok) std::printf("provider_rva=%lx capability_rva=%llx path_rva=%llx\n",f->BeginAddress,(unsigned long long)(block+7-b),(unsigned long long)(block+20-b));
 }
 return ok ? calls[0]-36 : nullptr;
}
}

