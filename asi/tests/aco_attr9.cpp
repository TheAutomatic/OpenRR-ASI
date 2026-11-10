// SPDX-License-Identifier: MIT
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include "opennr_rr/d3d12.hpp"
extern "C" void rr_vk_load();
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <stdexcept>

int main(int argc,char** argv) {
    try {
        if(argc!=4)throw std::runtime_error("usage: aco_attr9 shader.spv case.bin output-dir");
        rr_vk_load();
        IDXGIFactory6* fac=nullptr;IDXGIAdapter1* ad=nullptr;ID3D12Device* dev=nullptr;
        if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac))) ||
           FAILED(fac->EnumAdapterByGpuPreference(0,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&ad))))
            throw std::runtime_error("adapter selection failed");
        DXGI_ADAPTER_DESC1 desc{};ad->GetDesc1(&desc);
        if(FAILED(D3D12CreateDevice(ad,D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&dev))))
            throw std::runtime_error("D3D12 creation failed");
        fac->Release();ad->Release();
        {
            onr::DeviceOptions opt{};opt.luid=reinterpret_cast<const uint8_t*>(&desc.AdapterLuid);opt.descriptors=true;
            onr::Device vk;std::string err;
            if(!vk.init(err,opt))throw std::runtime_error(err);
            onr::D3dDevice d;
            if(!d.init(dev,&vk,L"",err))throw std::runtime_error(err);
            std::filesystem::create_directories(argv[3]);
            auto out=d.create(64*24*4);
            struct PC {uint64_t dst;uint32_t taps[4],k[2];float expo,previous,scale;uint32_t noise;} pc{};
            static_assert(sizeof(PC)==48);
            pc.dst=out.addr;
            std::ifstream input(argv[2],std::ios::binary);input.read(reinterpret_cast<char*>(&pc)+8,40);
            if(input.gcount()!=40)throw std::runtime_error("bad case input");
            for(bool aco:{false,true}) {
                d.set_compiler(aco,aco);
                auto kernel=d.load(argv[1],32);
                if(kernel.aco!=aco)throw std::runtime_error("compiler selection mismatch");
                d.begin_own();d.dispatch(kernel,&pc,sizeof pc,1);d.submit_own();
                uint32_t words[64*24]{};d.download(out,words,sizeof words);
                const char* label=aco?"aco":"llpc";
                std::ofstream f(std::filesystem::path(argv[3])/(std::string(label)+".bin"),std::ios::binary);
                f.write(reinterpret_cast<const char*>(words),sizeof words);
                if(!f)throw std::runtime_error("write failed");
                for(unsigned i=0;i<24;++i){float value;memcpy(&value,&words[i],4);std::printf("%s %2u %08x %.12g\n",label,i,words[i],value);}
                for(unsigned lane=1;lane<64;++lane)for(unsigned i=0;i<24;++i)
                    if(words[lane*24+i]!=words[i])throw std::runtime_error("identical cases differ by lane");
                d.destroy(kernel);
            }
            d.destroy(out);
        }
        dev->Release();
        std::puts("Windows D3D12/PAL arithmetic diagnostic completed; no timing measurement.");
        return 0;
    } catch(const std::exception& e){std::fprintf(stderr,"attr9: %s\n",e.what());return 1;}
}
