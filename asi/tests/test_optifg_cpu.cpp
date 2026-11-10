#include "../src/optifg_provider_locator.hpp"
#include "../src/optifg_ngx_proxy.cpp"
#include <thread>
#include <array>
#include <atomic>
int main(){
 if(NVSDK_NGX_D3D12_Init(1,L"",nullptr,nullptr,static_cast<NVSDK_NGX_Version>(0x13))!=NVSDK_NGX_Result_FAIL_NotInitialized)return 20;
 if(NVSDK_NGX_D3D12_Init_with_ProjectID("test",0,"",L"",nullptr,nullptr,static_cast<NVSDK_NGX_Version>(0x13))!=NVSDK_NGX_Result_FAIL_NotInitialized)return 21;
 NVSDK_NGX_Parameter *legacy=nullptr,*again=nullptr;
 if(NVSDK_NGX_D3D12_GetParameters(&legacy)!=NVSDK_NGX_Result_Success)return 22;
 NVSDK_NGX_D3D12_GetParameters(&again);
 if(legacy!=again || NVSDK_NGX_D3D12_DestroyParameters(legacy)!=NVSDK_NGX_Result_FAIL_InvalidParameter)return 23;
 NVSDK_NGX_FeatureDiscoveryInfo discovery{}; NVSDK_NGX_FeatureRequirement requirement{};
 discovery.FeatureID=NVSDK_NGX_Feature_SuperSampling;
 if(NVSDK_NGX_D3D12_GetFeatureRequirements(nullptr,&discovery,&requirement)!=NVSDK_NGX_Result_FAIL_FeatureNotSupported)return 24;
 unsigned char p[]={0x80,0xbd,0xb5,0x30,0,0,0,0x0f,0x84,0x6f,0xff,0xff,0xff,0x80,0xbf,0xb8,5,0,0,0,0x0f,0x84,0x62,0xff,0xff,0xff,0x4c,0x8d,0x85,0x10,0x30,0,0,0x48,0x8d,0x54,0x24,0x30,0x48,0x8d,0x4c,0x24,0x40};
 auto valid=[&]{return optifg_probe::validate_block(p,700,100);};
 if(!valid())return 1;
 for(int i: {2,3,4,5,15,16,17,18,29,30,31,32,37,42})p[i]^=0x55;
 if(!valid())return 2;
 int32_t a=-200,b=-213; memcpy(p+9,&a,4);memcpy(p+22,&b,4);
 if(!valid())return 3;
 p[22]++;if(valid())return 4;p[22]--;
 p[8]=0x85;if(valid())return 5;p[8]=0x84;
 if(optifg_probe::validate_block(p,50,100))return 6;
 NVSDK_NGX_Parameter* table=nullptr;
 if(NVSDK_NGX_D3D12_AllocateParameters(&table)!=NVSDK_NGX_Result_Success)return 7;
 std::atomic<bool> fail{false};std::array<std::thread,4> workers;
 for(int k=0;k<4;k++)workers[k]=std::thread([&,k]{for(unsigned i=0;i<20000;i++){
  if(k==3){reset(table);continue;}
  set<unsigned int>(table,"shared",i); unsigned out=0;auto r=get<unsigned int>(table,"shared",&out);
  if(r!=NVSDK_NGX_Result_Success && r!=NVSDK_NGX_Result_FAIL_UnsupportedParameter)fail=true;
 }});
 for(auto& t:workers)t.join();
 if(fail || NVSDK_NGX_D3D12_DestroyParameters(table)!=NVSDK_NGX_Result_Success)return 8;
 puts("PASS displacement mutations, negative branches, concurrent set/get/reset (80000 iterations)");
}
