// SPDX-License-Identifier: MIT
// Correctness diagnostics on the host's actual list. Readbacks are consumed only
// after the benchmark has waited for its fence; no replay or resource restoration.
#pragma once
#include <fstream>
#include <sstream>
#include <cstddef>

// Keep the CPU comparison script's pointer-slot schema tied to this ABI.
static_assert(offsetof(onr::EncBlockPC,mask)==112 && offsetof(onr::EncStagePC,dfn_w)==120);
static_assert(offsetof(onr::Enc0StagePC,attr)==200 && offsetof(onr::Enc0AttrPC,disocc)==208);
static_assert(offsetof(onr::DecDfnPC,ajab)==176 && offsetof(onr::DecDfnPC,tile)==224);
static_assert(offsetof(onr::PostDltPC,dh_in)==184 && offsetof(onr::AePC,exposure)==40);

namespace onr {
struct EngineRuntimeTraceAccess {
    static uint64_t phase(const Engine& e) { return e.frame_; }
    static bool fresh(const Engine& e) { return e.fresh_; }
    static bool sampled(const Engine& e) {return e.fast_ && e.sampler_ && e.hist_img_[0].img && e.hist_img_[1].img;}
    static bool flow(const Engine& e) {
        bool on=e.flow_on_ && e.fast_ && (e.pad_.y>>2)/8+1<=240;
        for(const auto& n:Engine::flow_kernel_names())on=on && e.k_->has(n);
        return on;
    }
    static void align_phase(Engine& e) {e.frame_=0;e.fresh_=true;}
    static std::vector<Buffer> roots(const Engine& e) {
        auto v=e.sized_; v.insert(v.end(),e.fixed_.begin(),e.fixed_.end());
        for (auto b:e.hist_) if(b.addr) v.push_back(b);
        for (auto b:e.dh_) if(b.addr) v.push_back(b);
        for (auto b:e.tlh_) if(b.addr) v.push_back(b);
        for (auto b:e.toh_) if(b.addr) v.push_back(b);
        return v;
    }
    static std::vector<std::pair<std::string,Buffer>> outputs(const Engine& e, const std::string& n) {
        std::vector<std::pair<std::string,Buffer>> v;
        auto add=[&](std::string name,Buffer b){if(b.addr && b.size)v.emplace_back(name,b);};
        const int cur=int(e.frame_&1);
        if(n=="frame-input") {
            const int old=cur^1;
            add("previous-motion",e.pmv2_[old]);add("previous-hidden",e.hidden_[old]);
            add("previous-down",e.down_[old]);add("previous-history",e.hist_[old]);add("previous-exposure",e.expo_[old]);
        } else if(n.find("pooled_history2")!=std::string::npos) add("pooled-history",e.pdst_);
        else if(n=="ae" || n=="d3d_ae") {add("exposure",e.expo_[cur]);add("exposure-block",e.blk_);}
        else if(n.find("enc0_attr")!=std::string::npos) {
            add("attributes",e.attr_);add("image",e.img_);add("motion",e.pmv2_[cur]);add("eroded",e.ero_);add("nondilated",e.nond_);
            add("parameters",e.prm_);add("guide-table",e.gtab_);
        } else if(n.find("enc0_stage")!=std::string::npos || n.find("enc_stage")!=std::string::npos) {
            for(int s=0;s<6;++s) {
                const auto& w=e.w_->enc[s];
                auto prefix=s?"enc_stage_"+std::to_string(w.b.C)+"_"+std::to_string(w.b.X)+"_"+std::to_string(w.Co):"enc0_stage_32_64";
                if(n.rfind(prefix,0)!=0)continue;
                add("tensor",e.enc_l_[s].tensor);add("dfn",e.enc_l_[s].dfn);
                add("pooled",e.enc_l_[s].pooled);add("dfn-pool",e.enc_l_[s].dfn_pool);
            }
        } else if(n.find("dec_up")!=std::string::npos || n.find("dec_dfn")!=std::string::npos || n.find("dec_head")!=std::string::npos) {
            for(int s=0;s<5;++s) {
                const bool up=n.rfind("dec_up",0)==0;const auto& w=up?e.w_->dec0[s]:e.w_->dec1[s];
                auto prefix=std::string(up?"dec_up_":s?"dec_dfn_":"dec_head_")+std::to_string(w.b.C)+"_"+std::to_string(w.b.X);
                if(up)prefix+="_"+std::to_string(w.Cp);
                if(n.rfind(prefix,0)!=0)continue;
                if(up)add("tensor",e.dec0_l_[s].tensor);
                else {add("tensor",e.dec1_l_[s].tensor);add("dfn",e.dec1_l_[s].dfn);}
                if(!up && !s) {add("tile",e.tile_);add("hidden",e.hidden_[cur]);add("aniso",e.aniso_);add("ajab",e.ajab_);add("mvf",e.mvf_);}
            }
        } else if(n.rfind("post",0)==0) {add("history",e.hist_[cur]);add("down",e.down_[cur]);add("rg",e.frg_);add("b",e.fb_);}
        return v;
    }
};
struct D3dRuntimeTraceAccess {
    static auto& hook(D3dDevice& d) {return d.dispatch_trace_;}
    static void descriptors(D3dDevice& d,std::ostream& o) {
        o<<"sampled-descriptors=";
        constexpr char hex[]="0123456789abcdef";
        for(auto& slot:d.tex_table_)for(auto b:slot)o<<hex[b>>4]<<hex[b&15];
        o<<'\n';
    }
};

struct RuntimeTrace {
    struct Copy {std::string name; ID3D12Resource* rb; uint64_t bytes; uint32_t pitch=0,row_bytes=0,rows=0;};
    std::filesystem::path path;
    std::vector<Copy> copies;
    std::ofstream meta;
    std::vector<Buffer> roots;
    unsigned dispatch=0;
    ~RuntimeTrace(){clear();}
    void clear(){for(auto& c:copies)c.rb->Release();copies.clear();meta.close();path.clear();}
    void texture(D3dDevice& d,const std::string& name,ID3D12Resource* src,uint32_t state,uint32_t x,uint32_t y,uint32_t w,uint32_t h) {
        auto desc=src->GetDesc();desc.Width=w;desc.Height=h;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT rows=0;UINT64 row_bytes=0,bytes=0;
        d.d3d()->GetCopyableFootprints(&desc,0,1,0,&fp,&rows,&row_bytes,&bytes);
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;
        bd.Height=1;bd.DepthOrArraySize=bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* rb=nullptr;
        if(FAILED(d.d3d()->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&rb))))
            throw std::runtime_error("runtime trace: texture readback allocation");
        copies.push_back({name,rb,bytes,fp.Footprint.RowPitch,uint32_t(row_bytes),rows});
        D3D12_RESOURCE_BARRIER t{};t.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        t.Transition={src,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATES(state),D3D12_RESOURCE_STATE_COPY_SOURCE};
        if(state!=D3D12_RESOURCE_STATE_COPY_SOURCE)d.list()->ResourceBarrier(1,&t);
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=rb;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
        D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=src;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_BOX box{x,y,0,x+w,y+h,1};d.list()->CopyTextureRegion(&dst,0,0,0,&from,&box);
        std::swap(t.Transition.StateBefore,t.Transition.StateAfter);
        if(state!=D3D12_RESOURCE_STATE_COPY_SOURCE)d.list()->ResourceBarrier(1,&t);
        meta<<"texture "<<name<<" format="<<desc.Format<<" width="<<w<<" height="<<h<<" row-bytes="<<row_bytes<<" origin="<<x<<','<<y<<'\n';
    }
    void snapshot(D3dDevice& d,const std::string& name,Buffer b) {
        if(!b.addr || !b.size)return;
        // Subviews deliberately have no COM resource. Resolve them against the
        // owning allocation rather than dropping the tensors from the trace.
        const Buffer* root=nullptr;
        for(auto& r:roots)if(r.res && b.addr>=r.addr && b.addr-r.addr<=r.size && b.size<=r.size-(b.addr-r.addr)){root=&r;break;}
        if(!root)throw std::runtime_error("runtime trace: unresolved view "+name);
        auto* src=static_cast<ID3D12Resource*>(root->res);
        const uint64_t off=b.addr-src->GetGPUVirtualAddress();
        if(off>src->GetDesc().Width || b.size>src->GetDesc().Width-off)throw std::runtime_error("runtime trace: view bounds");
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=b.size;
        desc.Height=1;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* rb=nullptr;
        if(FAILED(d.d3d()->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&rb))))
            throw std::runtime_error("runtime trace: readback allocation");
        copies.push_back({name,rb,b.size});
        D3D12_RESOURCE_BARRIER t{};t.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        t.Transition={src,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE};
        d.list()->ResourceBarrier(1,&t);d.list()->CopyBufferRegion(rb,0,src,off,b.size);
        std::swap(t.Transition.StateBefore,t.Transition.StateAfter);d.list()->ResourceBarrier(1,&t);
        meta<<"view "<<name<<" address="<<b.addr<<" bytes="<<b.size<<" root="<<root->addr<<" offset="<<off<<'\n';
    }
    void begin(D3dDevice& d,Engine& e) {
        std::filesystem::create_directories(path);meta.open(path/"trace.txt");
        if(!meta)throw std::runtime_error("runtime trace: open metadata");
        dispatch=0;roots=EngineRuntimeTraceAccess::roots(e);
        if(const char* record_dir=getenv("ONR_ACO_DIR")) {
            const auto manifest=u8_to_path(record_dir)/"manifest.json";
            if(std::filesystem::exists(manifest))std::filesystem::copy_file(manifest,path/"aco-manifest.json",std::filesystem::copy_options::overwrite_existing);
            meta<<"records="<<record_dir<<'\n';
        }
        D3dRuntimeTraceAccess::descriptors(d,meta);
        for(auto& [name,b]:EngineRuntimeTraceAccess::outputs(e,"frame-input"))snapshot(d,"input-"+name,b);
        D3dRuntimeTraceAccess::hook(d)=[this,&d,&e](const Kernel& k,const void* pc,uint32_t size,uint32_t x,uint32_t y,uint32_t z,bool indirect){
            const auto n=u8_to_path(k.name).stem().string();const auto tag=std::to_string(dispatch++)+"-"+n;
            meta<<"dispatch "<<tag<<" aco="<<k.aco<<" indirect="<<indirect<<" groups="<<x<<','<<y<<','<<z<<" push=";
            constexpr char hex[]="0123456789abcdef";
            for(unsigned i=0;i<size;++i){auto b=static_cast<const uint8_t*>(pc)[i];meta<<hex[b>>4]<<hex[b&15];}meta<<'\n';
            if(n=="ingest" || n=="d3d_ingest") {
                // IngestPC has destinations at byte 24. Copy only non-null
                // destinations: direct guides may bypass this dispatch entirely.
                for(unsigned s=0;s<8 && 24+8*(s+1)<=size;++s){
                    uint64_t addr=0;memcpy(&addr,static_cast<const uint8_t*>(pc)+24+8*s,8);
                    for(auto r:roots)if(r.addr==addr){snapshot(d,tag+"-guide-"+std::to_string(s),r);break;}
                }
            }
            // A selected dispatch avoids serializing every stage in the flow
            // chain when the full trace changes the reproduced difference.
            const char* filter=getenv("ONR_RR_TRACE_KERNEL");
            if(!filter || n==filter)for(auto& [name,b]:EngineRuntimeTraceAccess::outputs(e,n))snapshot(d,tag+"-"+name,b);
        };
    }
    void finish() {
        for(auto& c:copies) {
            void* p=nullptr;D3D12_RANGE range{0,size_t(c.bytes)};
            if(FAILED(c.rb->Map(0,&range,&p)))throw std::runtime_error("runtime trace: map");
            std::ofstream f(path/(c.name+".bin"),std::ios::binary);
            if(c.rows)for(uint32_t y=0;y<c.rows;++y)f.write(static_cast<const char*>(p)+uint64_t(y)*c.pitch,c.row_bytes);
            else f.write(static_cast<const char*>(p),std::streamsize(c.bytes));
            D3D12_RANGE empty{};c.rb->Unmap(0,&empty);
            if(!f)throw std::runtime_error("runtime trace: write");
        }
        clear();
    }
};
} // namespace onr
