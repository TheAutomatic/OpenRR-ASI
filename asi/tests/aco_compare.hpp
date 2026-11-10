// SPDX-License-Identifier: MIT
// Diagnostic only: compare intermediate buffers from independent, identical frames.
#include "rr_pal_binary.hpp"
#include <immintrin.h>
#include <type_traits>
namespace onr {
struct D3dTestAccess {
    static auto& hook(D3dDevice& d) { return d.dispatch_probe_; }
    static auto& buffers(D3dDevice& d) { return d.probe_buffers_; }
};
struct EngineTestAccess {
    static std::vector<std::pair<std::string, Buffer>> buffers(const Engine& e) {
        std::vector<std::pair<std::string, Buffer>> v;
        auto add=[&](std::string n, const Buffer& b){if(b.res && b.size)v.emplace_back(n,b);};
        const int cur=int((e.frame_+1)&1);
        add("exposure",e.expo_[cur]); add("attr",e.attr_);add("img",e.img_);
        add("pmv",e.pmv2_[cur]);add("hidden",e.hidden_[cur]);add("ero",e.ero_);add("nond",e.nond_);
        for(int i=0;i<6;++i){add("enc"+std::to_string(i)+".tensor",e.enc_l_[i].tensor);add("enc"+std::to_string(i)+".dfn",e.enc_l_[i].dfn);add("enc"+std::to_string(i)+".pool",e.enc_l_[i].pooled);}
        for(int i=4;i>=0;--i){add("dec"+std::to_string(i)+".tensor",e.dec1_l_[i].tensor);add("dec"+std::to_string(i)+".dfn",e.dec1_l_[i].dfn);}
        add("history",e.hist_[cur]);add("down",e.down_[cur]);return v;
    }
};
}

static int aco_compare(onr::Device& vk, onr::D3dDevice& d, const std::string& dir) {
    if(getenv("OPENRR_ACO_MATH")) {
        d.set_compiler(false);auto ref=d.load(dir+"/aco_math.spv",32);
        d.set_compiler(true,true);auto test=d.load(dir+"/aco_math.spv",32);
        auto dst=d.create(65536*8);struct{uint64_t dst;uint32_t noise,mode;}pc{dst.addr,127,0};
        std::vector<uint32_t> out[2];
        for(int c=0;c<2;++c){d.begin_own();d.dispatch(c?test:ref,&pc,sizeof pc,1024);d.submit_own();out[c].resize(65536*2);d.download(dst,out[c].data(),65536*8);}
        size_t diff=0;for(unsigned i=0;i<65536*2;++i)if(out[0][i]!=out[1][i]){if(diff<24)printf("[MATH] h=%04x vector=%u LLPC=%08x ACO=%08x\n",i/2,i%2,out[0][i],out[1][i]);++diff;}
        printf("[MATH] total_diff=%zu\n",diff);return diff?1:0;
    }
    onr::Weights w;std::string err;
    const char* dll=getenv("OPENRR_DLSSD");
    if(!onr::build_weights(onr::u8_to_path(dll?dll:"C:/Users/Kenny/Downloads/dlssnr_on_amd_setup/dist/weights/nvngx_dlssd.dll").wstring(),w,err))throw std::runtime_error(err);
    const bool isolated=getenv("OPENRR_ACO_ISOLATED")!=nullptr;
    const bool fast=!getenv("OPENRR_ACO_LEGACY");
    const char* flow_option=getenv("ONR_RR_FLOW");
    const bool flow=!isolated && (!flow_option || std::strcmp(flow_option,"0")!=0);
    const bool sampled=!isolated && !getenv("ONR_RR_NO_SAMPLER");
    if(sampled && !d.init_sampler(err))throw std::runtime_error("diagnostic sampler: "+err);
    // The runtime always loads _f as its base, then _lt and flow variants.
    auto names=onr::Engine::kernel_names(w,true);names.push_back("clear_range");
    if(!isolated){auto f=onr::Engine::flow_kernel_names();names.insert(names.end(),f.begin(),f.end());}
    onr::Kernels llpc,aco,lite[2];
    d.set_compiler(false);llpc.load(d,dir,names);
    if(fast && !isolated)lite[0].load_lite(d,dir,names);
    d.set_compiler(true,true);aco.load(d,dir,names);
    if(fast && !isolated)lite[1].load_lite(d,dir,names);
    auto select=[&](int c){
        d.set_compiler(c!=0);
        // set_compiler retires the old internal fill kernel. Reinstall the
        // matching one even though the external kernel sets are already loaded.
        auto clear=d.load(dir+"/clear_range.spv");d.destroy(clear);
    };
    auto configure=[&](onr::Engine& e,int c){
        select(c);e.init(d,c?aco:llpc,w);e.set_fast(true);
        if(fast && !isolated){e.set_lite(&lite[c]);e.set_lite_on(true);}
        e.set_flow(flow);e.set_history_sampler(sampled);
    };
    printf("[CONFIG] path=%s mode=%s flow=%d sampler=%d lite=%zu/%zu\n",
           isolated?"base-isolated":"runtime-network",fast?"Fast":"Legacy",flow,sampled,lite[0].names().size(),lite[1].names().size());
    if(getenv("OPENRR_ACO_PROFILE")) {
        d.set_stamps(true);
        const size_t px=1280*720;auto col=d.create(px*8),alb=d.create(px*4),spc=d.create(px*4),nrm=d.create(px*8),rgh=d.create(px*2),dep=d.create(px*4),mv=d.create(px*4),hit=d.create(px*2),out=d.create(1920*1080*8);
        d.fill(col,0x38003800);d.fill(alb,0x80808080);d.fill(spc,0x20202020);d.fill(nrm,0x00003c00);d.fill(rgh,0x38003800);d.fill(dep,0x3f800000);d.fill(mv,0);d.fill(hit,0x38003800);
        onr::Guides g{col.addr,alb.addr,spc.addr,nrm.addr,rgh.addr,dep.addr,mv.addr,hit.addr};
        onr::EngineFrame f{};f.render={1280,720};f.output={1920,1080};
        for(int c=0;c<2;++c) {
            onr::Engine e;configure(e,c);e.resize({1280,720},{1920,1080});
            std::map<std::string,std::pair<double,unsigned>> times;
            for(int frame=0;frame<24;++frame) {
                f.reset=frame==0;d.begin_own();e.record(f,g,out.addr);
                auto stamp_names=d.stamp_names;std::vector<uint64_t> ts(d.queries());d.submit_own();
                if(frame<8)continue;
                if(!d.read_stamps(onr::D3dDevice::kOwnSlot,ts))throw std::runtime_error("profile timestamps missing");
                for(auto& s:stamp_names) {auto at=s.find_last_of('@');auto n=std::stoul(s.substr(at+1));
                    if(n+1>=ts.size())throw std::runtime_error("profile query range");
                    auto& t=times[s.substr(0,at)];t.first+=double(ts[n+1]-ts[n])*1000.0/d.timestamp_frequency();++t.second;
                }
            }
            for(auto& [name,t]:times)printf("[PROFILE] compiler=%s path=%s mode=%s flow=%d sampler=%d %s mean_ms=%.6f samples=%u\n",c?"ACO":"LLPC",isolated?"base-isolated":"runtime-network",fast?"Fast":"Legacy",flow,sampled,name.c_str(),t.first/t.second,t.second);
            e.destroy();
        }
        llpc.unload(d);aco.unload(d);for(auto& k:lite)k.unload(d);return 0;
    }
    if(getenv("OPENRR_ACO_DISPATCH_COMPARE")) {
        using Hook=std::function<void(const onr::Kernel&,const void*,uint32_t,uint32_t,uint32_t,uint32_t)>;
        unsigned sequence=0;
        size_t dispatch_differences=0;
        std::map<std::string,bool> saved_differences;
        onr::Kernel raw[2];
        if(getenv("OPENRR_ACO_RAW")) {
            d.set_compiler(false);raw[0]=d.load(dir+"/enc0_attr_raw.spv",32);
            d.set_compiler(true,true);raw[1]=d.load(dir+"/enc0_attr_raw.spv",32);
        }
        Hook hook;
        hook=[&](const onr::Kernel& k,const void* pc,uint32_t size,uint32_t x,uint32_t y,uint32_t z){
            onr::D3dTestAccess::hook(d)={};
            auto kernel_name=onr::u8_to_path(k.name).stem().string();
            if (getenv("OPENRR_ACO_ATTR_ONLY") && kernel_name != "enc0_attr_f") {
                d.dispatch(k,pc,size,x,y,z);onr::D3dTestAccess::hook(d)=hook;return;
            }
            d.submit_own();
            std::vector<onr::Buffer> buffers;
            const auto& all=onr::D3dTestAccess::buffers(d);
            for(unsigned off=0;off+8<=size;off+=4){
                uint64_t address;memcpy(&address,(const uint8_t*)pc+off,8);
                auto it=all.upper_bound(address);if(it==all.begin())continue;--it;
                if(address-it->first>=it->second.size)continue;
                if(std::none_of(buffers.begin(),buffers.end(),[&](auto& b){return b.addr==it->first;}))buffers.push_back(it->second);
            }
            std::vector<std::vector<uint8_t>> before,base;
            for(auto& b:buffers){std::vector<uint8_t> bytes(b.size);d.download(b,bytes.data(),b.size);before.push_back(std::move(bytes));}
            if(kernel_name=="dec_head_32_64_f") {
                onr::DecDfnPC p;memcpy(&p,pc,sizeof p);
                for(size_t i=0;i<buffers.size();++i) {
                    auto& b=buffers[i];
                    printf("[HEAD-PC] seq=%u b%zu=%llx size=%llu mvf=%llx hidden=%llx aniso=%llx tile=%llx dfn=%llx\n",sequence,i,(unsigned long long)b.addr,(unsigned long long)b.size,(unsigned long long)p.mvf,(unsigned long long)p.hidden,(unsigned long long)p.aniso,(unsigned long long)p.tile,(unsigned long long)p.dfn_out);
                }
            }
            if(raw[0].pso && kernel_name=="enc0_attr_f") {
                onr::Enc0AttrPC params;memcpy(&params,pc,sizeof params);
                for(size_t i=0;i<buffers.size();++i) {
                    const auto& b=buffers[i];
                    if(params.expo>=b.addr && params.expo+16<=b.addr+b.size) {
                        float v[4];memcpy(v,before[i].data()+params.expo-b.addr,16);
                        printf("[RAW-PC] frame=%u expo=%llx allocation=%zu+%llu values=%.9g,%.9g,%.9g,%.9g attr=%llx pmv2=%llx ero=%llx nond=%llx dfn=%llx\n",sequence,(unsigned long long)params.expo,i,(unsigned long long)(params.expo-b.addr),v[0],v[1],v[2],v[3],(unsigned long long)params.attr,(unsigned long long)params.pmv2,(unsigned long long)params.ero,(unsigned long long)params.nond,(unsigned long long)params.dfn);
                    }
                }
                auto dst=d.create(uint64_t(params.pad.x)*params.pad.y*144);params.attr=dst.addr;
                std::vector<uint16_t> values[2];
                for(int c=0;c<2;++c){
                    for(size_t i=0;i<buffers.size();++i)d.upload(buffers[i],before[i].data(),before[i].size());
                    d.begin_own();d.dispatch(raw[c],&params,size,x,y,z);d.submit_own();values[c].resize(dst.size/2);d.download(dst,values[c].data(),dst.size);
                    for(const auto& b:buffers)if(params.expo>=b.addr && params.expo+16<=b.addr+b.size) {
                        float v[4];d.download(b,v,16,params.expo-b.addr);
                        printf("[RAW-PC] frame=%u compiler=%d expo_after=%.9g,%.9g,%.9g,%.9g\n",sequence,c,v[0],v[1],v[2],v[3]);
                    }
                }
                size_t differences=0;std::map<unsigned,size_t> channels;
                const size_t raw_offset=size_t(params.pad.x)*params.pad.y*24;
                for(unsigned c=0;c<48;++c)printf("[RAW-PIXEL] frame=%u attr=%u LLPC=%04x ACO=%04x\n",sequence,c,values[0][raw_offset+c],values[1][raw_offset+c]);
                for(size_t i=raw_offset;i<values[0].size();++i)if(values[0][i]!=values[1][i]){if(differences<20)printf("[RAW] pixel=%zu attr=%zu LLPC=%04x ACO=%04x\n",(i-raw_offset)/48,(i-raw_offset)%48,values[0][i],values[1][i]);++differences;++channels[(i-raw_offset)%48];}
                printf("[RAW] frame=%u total_diff=%zu",sequence,differences);for(auto [c,n]:channels)printf(" attr%u=%zu",c,n);printf("\n");fflush(stdout);d.destroy(dst);
                for(size_t i=0;i<buffers.size();++i)d.upload(buffers[i],before[i].data(),before[i].size());
            }
            d.begin_own();d.dispatch(k,pc,size,x,y,z);d.submit_own();
            for(auto& b:buffers){std::vector<uint8_t> bytes(b.size);d.download(b,bytes.data(),b.size);base.push_back(std::move(bytes));}
            for(size_t i=0;i<buffers.size();++i)d.upload(buffers[i],before[i].data(),before[i].size());
            auto name=onr::u8_to_path(k.name).stem().string();
            const bool is_lite=name.size()>3 && name.substr(name.size()-3)=="_lt";
            auto key=is_lite?name.substr(0,name.size()-3):name;
            d.begin_own();d.dispatch(is_lite?lite[1][key]:aco[key],pc,size,x,y,z);d.submit_own();
            size_t total=0;
            for(size_t i=0;i<buffers.size();++i){
                auto& b=buffers[i];std::vector<uint8_t> bytes(b.size);d.download(b,bytes.data(),b.size);
                size_t diff=0,first=bytes.size();for(size_t j=0;j<bytes.size();++j)if(base[i][j]!=bytes[j]){++diff;first=std::min(first,j);}
                total+=diff;
                if(diff){
                    printf("[DISPATCH] #%u %s buffer=%zu size=%zu differing=%zu first=%zu\n",sequence,name.c_str(),i,bytes.size(),diff,first);fflush(stdout);
                    auto dir=std::filesystem::path("build/aco-tools/dispatch-compare");std::filesystem::create_directories(dir);
                    const auto dump_key=name+"-b"+std::to_string(i);
                    if(!saved_differences[dump_key]) {
                        for(int c=0;c<3;++c){auto& v=c==0?before[i]:c==1?base[i]:bytes;std::ofstream file(dir/(std::to_string(sequence)+"-"+name+"-b"+std::to_string(i)+"-"+std::to_string(c)+".bin"),std::ios::binary);file.write((char*)v.data(),v.size());}
                        saved_differences[dump_key]=true;
                    }
                }
                d.upload(b,base[i].data(),base[i].size());
            }
            printf("[DISPATCH] #%u %s total_diff=%zu buffers=%zu\n",sequence++,name.c_str(),total,buffers.size());fflush(stdout);
            dispatch_differences+=total;
            onr::D3dTestAccess::hook(d)=hook;d.begin_own();
        };
        onr::D3dTestAccess::hook(d)=hook;
        onr::Engine e;configure(e,0);e.resize({1280,720},{1920,1080});
        // Every dispatch receives precisely the same memory contents and GPU addresses.
        const size_t px=1280*720;auto col=d.create(px*8),alb=d.create(px*4),spc=d.create(px*4),nrm=d.create(px*8),rgh=d.create(px*2),dep=d.create(px*4),mv=d.create(px*4),hit=d.create(px*2),out=d.create(1920*1080*8);
        d.fill(col,0x38003800);d.fill(alb,0x80808080);d.fill(spc,0x20202020);d.fill(nrm,0x00003c00);d.fill(rgh,0x38003800);d.fill(dep,0x3f800000);d.fill(mv,0);d.fill(hit,0x38003800);
        std::vector<uint16_t> input(px*4),motion(px*2),distance(px);
        std::vector<float> depth(px);
        for(size_t i=0;i<px;++i){
            unsigned x=i%1280,y=i/1280;float u=float(x)/1280,v=float(y)/720;
            float checker=(((x/16)+(y/16))&1)?0.75f:1.f;
            float noise=float(((i*1103515245u+12345u)>>16)&255)/255.f*0.15f;
            input[4*i]=_cvtss_sh((0.4f*checker+noise)*(0.5f+0.5f*u),0);
            input[4*i+1]=_cvtss_sh((0.35f*checker+noise)*(0.5f+0.5f*v),0);
            input[4*i+2]=_cvtss_sh(0.25f*checker+noise,0);input[4*i+3]=0x3c00;
            motion[2*i]=_cvtss_sh(0.25f,0);motion[2*i+1]=_cvtss_sh(-0.15f,0);
            distance[i]=_cvtss_sh(2.5f+5.f*v,0);depth[i]=0.1f+0.8f*u;
        }
        d.upload(col,input.data(),input.size()*2);
        d.upload(mv,motion.data(),motion.size()*2);d.upload(hit,distance.data(),distance.size()*2);d.upload(dep,depth.data(),depth.size()*4);
        onr::Guides g{col.addr,alb.addr,spc.addr,nrm.addr,rgh.addr,dep.addr,mv.addr,hit.addr};
        onr::EngineFrame f{};f.render={1280,720};f.output={1920,1080};f.reset=true;
        for(int frame=0;frame<10;++frame){
            f.render=frame>=4 && frame<8?onr::I2{1088,612}:onr::I2{1280,720};
            // Guides use current render-width rows. Crop the original rectangle
            // row by row, rather than reinterpreting a 1280-wide buffer at DRS.
            auto repack=[&](const auto& src,auto& dst,unsigned components){
                using T=typename std::decay_t<decltype(src)>::value_type;
                std::vector<T> tight(size_t(f.render.x)*f.render.y*components);
                for(int y=0;y<f.render.y;++y)std::copy_n(src.data()+size_t(y)*1280*components,size_t(f.render.x)*components,tight.data()+size_t(y)*f.render.x*components);
                d.upload(dst,tight.data(),tight.size()*sizeof(T));
            };
            repack(input,col,4);repack(motion,mv,2);repack(distance,hit,1);repack(depth,dep,1);
            f.reset=frame==0 || frame==4 || frame==8;e.resize(f.render,f.output);
            printf("[FRAME] %d render=%dx%d reset=%d\n",frame,f.render.x,f.render.y,f.reset);
            d.begin_own();e.record(f,g,out.addr);d.submit_own();
        }
        onr::D3dTestAccess::hook(d)={};e.destroy();llpc.unload(d);aco.unload(d);for(auto& k:lite)k.unload(d);
        printf("[DISPATCH] compared=%u differing_bytes=%zu\n",sequence,dispatch_differences);
        return dispatch_differences?1:0;
    }
    auto dump=std::filesystem::path("build/aco-tools/compare");std::filesystem::create_directories(dump);
    for(auto& n:names) {
        auto binary=vk.pipeline_elf(dir+"/"+n+".spv",32);
        std::ofstream f(dump/(n+".elf"),std::ios::binary);f.write((char*)binary.data(),binary.size());
    }
    onr::Engine engines[2];
    for(int c=0;c<2;++c){printf("[COMPARE] init=%d\n",c);fflush(stdout);configure(engines[c],c);engines[c].resize({1280,720},{1920,1080});}
    const size_t px=1280*720;
    auto col=d.create(px*8),alb=d.create(px*4),spc=d.create(px*4),nrm=d.create(px*8),rgh=d.create(px*2),dep=d.create(px*4),mv=d.create(px*4),hit=d.create(px*2);
    auto out=d.create(1920*1080*8);
    std::vector<uint16_t> colors(px*4);
    for(size_t i=0;i<px;++i){colors[i*4]=0x3000+(i%257);colors[i*4+1]=0x3400+(i%127);colors[i*4+2]=0x3800+(i%63);colors[i*4+3]=0x3c00;}
    d.upload(col,colors.data(),px*8);d.fill(alb,0x80808080);d.fill(spc,0x20202020);d.fill(nrm,0x00003c00);d.fill(rgh,0x38003800);d.fill(dep,0x3f800000);d.fill(mv,0);d.fill(hit,0x38003800);
    onr::Guides guides{col.addr,alb.addr,spc.addr,nrm.addr,rgh.addr,dep.addr,mv.addr,hit.addr};
    onr::EngineFrame frame{};frame.render={1280,720};frame.output={1920,1080};frame.reset=true;
    for(int i=0;i<16;++i)frame.world_to_view[i]=frame.view_to_clip[i]=(i%5==0)?1.f:0.f;
    size_t frame_differences=0;
    for(int f=0;f<10;++f){
        frame.render=f>=4 && f<8?onr::I2{1088,612}:onr::I2{1280,720};
        std::vector<std::pair<std::string,std::vector<uint8_t>>> snapshots[2];
        for(int c=0;c<2;++c){
            select(c);
            std::vector<uint16_t> tight(size_t(frame.render.x)*frame.render.y*4);
            for(int y=0;y<frame.render.y;++y)std::copy_n(colors.data()+size_t(y)*1280*4,size_t(frame.render.x)*4,tight.data()+size_t(y)*frame.render.x*4);
            d.upload(col,tight.data(),tight.size()*2);
            engines[c].resize(frame.render,frame.output);
            printf("[COMPARE] record frame=%d compiler=%d\n",f,c);fflush(stdout);
            frame.reset=f==0 || f==4 || f==8;d.begin_own();engines[c].record(frame,guides,out.addr);d.submit_own();
            auto buffers=onr::EngineTestAccess::buffers(engines[c]);buffers.emplace_back("output",out);
            for(auto& [name,b]:buffers){
                auto offset=b.addr-static_cast<ID3D12Resource*>(b.res)->GetGPUVirtualAddress();
                auto rd=static_cast<ID3D12Resource*>(b.res)->GetDesc();
                if(offset>rd.Width || b.size>rd.Width-offset)throw std::runtime_error("snapshot outside allocation");
                size_t size=size_t(b.size);
                std::vector<uint8_t> bytes(size);d.download(b,bytes.data(),size,offset);
                snapshots[c].emplace_back(name,std::move(bytes));
            }
        }
        for(size_t i=0;i<snapshots[0].size();++i){
            auto& [name,a]=snapshots[0][i];auto& b=snapshots[1][i].second;
            size_t diff=0,first=a.size();for(size_t j=0;j<a.size();++j)if(a[j]!=b[j]){++diff;first=std::min(first,j);}
            frame_differences+=diff;
            printf("[COMPARE] frame=%d %s bytes=%zu diff=%zu first=%zu\n",f,name.c_str(),a.size(),diff,first);
            if(diff)for(int c=0;c<2;++c){auto& data=snapshots[c][i].second;std::ofstream file(dump/("f"+std::to_string(f)+"-"+name+(c?"-aco.bin":"-llpc.bin")),std::ios::binary);file.write((char*)data.data(),data.size());}
        }
    }
    // Process exits after this diagnostic; release engine-owned resources explicitly.
    for(auto& e:engines)e.destroy();
    llpc.unload(d);aco.unload(d);for(auto& k:lite)k.unload(d);
    return frame_differences?1:0;
}
