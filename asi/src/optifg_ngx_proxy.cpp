// Experimental D3D12 RR-only NGX companion. Not a general NVIDIA NGX replacement.
#include <windows.h>
#include "rr_ngx_abi.hpp"
#include <unordered_map>
#include <string>
#include <variant>
#include <mutex>
#include <unordered_set>
#include <memory>
using R=NVSDK_NGX_Result;
extern "C" __declspec(dllexport) unsigned int OpenRrNgxProxyMarker() { return 0x4f520001u; }
using Value=std::variant<void*,int,unsigned int,double,float,unsigned long long>;
struct Params { NVSDK_NGX_Parameter api; std::unordered_map<std::string,Value> values; mutable std::mutex values_mutex; };
template<class T> void RR_NGX_MSABI set(NVSDK_NGX_Parameter* p,const char* n,T v) {
 if(!p||!n)return; auto q=reinterpret_cast<Params*>(p);
 std::lock_guard<std::mutex> guard(q->values_mutex);
 if constexpr(std::is_pointer_v<T>) q->values[n]=reinterpret_cast<void*>(v); else q->values[n]=v;
}
template<class T> R RR_NGX_MSABI get(const NVSDK_NGX_Parameter* p,const char* n,T* out) {
 if(!p||!n||!out)return NVSDK_NGX_Result_FAIL_InvalidParameter;
 auto q=reinterpret_cast<const Params*>(p);
 std::lock_guard<std::mutex> guard(q->values_mutex); auto it=q->values.find(n);
 if(it==q->values.end())return NVSDK_NGX_Result_FAIL_UnsupportedParameter;
 bool ok=false; std::visit([&](auto v){
  using V=decltype(v);
  if constexpr(std::is_pointer_v<T> && std::is_pointer_v<V>) { *out=reinterpret_cast<T>(v);ok=true; }
  else if constexpr(!std::is_pointer_v<T> && !std::is_pointer_v<V>) { *out=static_cast<T>(v);ok=true; }
 },it->second); return ok?NVSDK_NGX_Result_Success:NVSDK_NGX_Result_FAIL_UnsupportedParameter;
}
void RR_NGX_MSABI reset(NVSDK_NGX_Parameter* p) {
 if(!p)return; auto q=reinterpret_cast<Params*>(p);
 std::lock_guard<std::mutex> guard(q->values_mutex); q->values.clear();
}
// Callers must finish accessing a parameter table before destroying it.
static const NVSDK_NGX_ParameterVtbl vt={set<void*>,set<ID3D12Resource*>,set<ID3D11Resource*>,set<int>,set<unsigned int>,set<double>,set<float>,set<unsigned long long>,get<void*>,get<ID3D12Resource*>,get<ID3D11Resource*>,get<int>,get<unsigned int>,get<double>,get<float>,get<unsigned long long>,reset};
static std::mutex mutex; static std::unordered_set<Params*> owned;
template<class T> T resolve(const char* name) {auto m=GetModuleHandleW(L"openrr-amd.asi"); return m?reinterpret_cast<T>(GetProcAddress(m,name)):nullptr;}
#define API extern "C" __declspec(dllexport) R RR_NGX_MSABI
using Create=R(RR_NGX_MSABI*)(ID3D12GraphicsCommandList*,NVSDK_NGX_Feature,NVSDK_NGX_Parameter*,NVSDK_NGX_Handle**);
using Eval=R(RR_NGX_MSABI*)(ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,NVSDK_NGX_Parameter*,void*);
using Release=R(RR_NGX_MSABI*)(NVSDK_NGX_Handle*);
static bool ready(){return resolve<Create>("OpenRrHostCreateD3D12")&&resolve<Eval>("OpenRrHostEvaluateD3D12")&&resolve<Release>("OpenRrHostReleaseD3D12");}
API NVSDK_NGX_D3D12_Init_ProjectID(const char*,int,const char*,const wchar_t*,ID3D12Device* dev,NVSDK_NGX_Version,const NVSDK_NGX_FeatureCommonInfo*){return dev&&ready()?NVSDK_NGX_Result_Success:NVSDK_NGX_Result_FAIL_NotInitialized;}
API NVSDK_NGX_D3D12_Init_Ext(unsigned long long,const wchar_t*,ID3D12Device* dev,NVSDK_NGX_Version,const NVSDK_NGX_FeatureCommonInfo*){return dev&&ready()?NVSDK_NGX_Result_Success:NVSDK_NGX_Result_FAIL_NotInitialized;}
// Public NGX entry points use FeatureInfo before SDKVersion, unlike the
// internal _Ext / _ProjectID entry points used by OptiScaler.
API NVSDK_NGX_D3D12_Init(unsigned long long app,const wchar_t* path,ID3D12Device* dev,const NVSDK_NGX_FeatureCommonInfo* info,NVSDK_NGX_Version version){
 return NVSDK_NGX_D3D12_Init_Ext(app,path,dev,version,info);
}
API NVSDK_NGX_D3D12_Init_with_ProjectID(const char* app,int engine,const char* engineVersion,const wchar_t* path,ID3D12Device* dev,const NVSDK_NGX_FeatureCommonInfo* info,NVSDK_NGX_Version version){
 return NVSDK_NGX_D3D12_Init_ProjectID(app,engine,engineVersion,path,dev,version,info);
}
API NVSDK_NGX_D3D12_AllocateParameters(NVSDK_NGX_Parameter** out){
 if(!out)return NVSDK_NGX_Result_FAIL_InvalidParameter;
 *out=nullptr;
 try {auto p=std::unique_ptr<Params>(new Params{{&vt},{}});std::lock_guard<std::mutex> lock(mutex);owned.insert(p.get());*out=&p.release()->api;return NVSDK_NGX_Result_Success;}catch(...){return NVSDK_NGX_Result_Fail;}
}
API NVSDK_NGX_D3D12_GetCapabilityParameters(NVSDK_NGX_Parameter** out){return NVSDK_NGX_D3D12_AllocateParameters(out);}
// Legacy table is runtime-owned, unlike Allocate/GetCapabilityParameters.
API NVSDK_NGX_D3D12_GetParameters(NVSDK_NGX_Parameter** out){
 if(!out)return NVSDK_NGX_Result_FAIL_InvalidParameter;
 static Params legacy{{&vt},{}}; *out=&legacy.api; return NVSDK_NGX_Result_Success;
}
API NVSDK_NGX_D3D12_GetFeatureRequirements(IDXGIAdapter*,const NVSDK_NGX_FeatureDiscoveryInfo* info,NVSDK_NGX_FeatureRequirement* out){
 if(!info||!out)return NVSDK_NGX_Result_FAIL_InvalidParameter;
 *out={};
 out->FeatureSupported=NVSDK_NGX_FeatureSupportResult_NotImplemented;
 // The companion implements RR only; ordinary SR remains OptiScaler-owned.
 if(info->FeatureID!=NVSDK_NGX_Feature_RayReconstruction)return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
 if(!ready())return NVSDK_NGX_Result_FAIL_NotInitialized;
 out->FeatureSupported=NVSDK_NGX_FeatureSupportResult_Supported;
 return NVSDK_NGX_Result_Success;
}
API NVSDK_NGX_D3D12_DestroyParameters(NVSDK_NGX_Parameter* api){std::lock_guard<std::mutex> lock(mutex);auto p=reinterpret_cast<Params*>(api);if(!owned.erase(p))return NVSDK_NGX_Result_FAIL_InvalidParameter;delete p;return NVSDK_NGX_Result_Success;}
API NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList* c,NVSDK_NGX_Feature f,NVSDK_NGX_Parameter* p,NVSDK_NGX_Handle** h){auto fn=resolve<Create>("OpenRrHostCreateD3D12");return fn?fn(c,f,p,h):NVSDK_NGX_Result_FAIL_NotInitialized;}
API NVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList* c,const NVSDK_NGX_Handle* h,NVSDK_NGX_Parameter* p,void* cb){auto fn=resolve<Eval>("OpenRrHostEvaluateD3D12");return fn?fn(c,h,p,cb):NVSDK_NGX_Result_FAIL_NotInitialized;}
API NVSDK_NGX_D3D12_ReleaseFeature(NVSDK_NGX_Handle* h){auto fn=resolve<Release>("OpenRrHostReleaseD3D12");return fn?fn(h):NVSDK_NGX_Result_FAIL_NotInitialized;}
API NVSDK_NGX_D3D12_Shutdown(){return NVSDK_NGX_Result_Success;}
API NVSDK_NGX_D3D12_Shutdown1(ID3D12Device*){return NVSDK_NGX_Result_Success;}
API NVSDK_NGX_D3D12_GetScratchBufferSize(NVSDK_NGX_Feature f,const NVSDK_NGX_Parameter*,size_t* out){if(!out)return NVSDK_NGX_Result_FAIL_InvalidParameter;*out=0;return f==NVSDK_NGX_Feature_RayReconstruction?NVSDK_NGX_Result_Success:NVSDK_NGX_Result_FAIL_FeatureNotSupported;}
