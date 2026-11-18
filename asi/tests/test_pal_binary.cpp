// SPDX-License-Identifier: MIT
#include "rr_pal_binary.hpp"
#include <cstdio>
#include <map>

using namespace onr::pal;
static Bytes serialize(const Record& r) {
    Bytes b(96); std::memcpy(b.data(), "NRPAL002", 8);
    write(b,8,r.spv_hash); write(b,16,hash(r.code.data(),r.code.size()));
    const uint32_t fields[]={r.rsrc1,r.rsrc2,r.rsrc3,r.exec,r.wg,r.shared,r.push_bytes,r.user};
    for(unsigned i=0;i<8;++i)write(b,24+4*i,fields[i]);
    write(b,56,int32_t(r.set0));write(b,60,int32_t(r.push));write(b,64,int32_t(r.grid));
    write(b,68,uint32_t(r.inlines.size()));write(b,72,uint32_t(r.code.size()));
    write(b,76,r.mach);write(b,80,r.scratch_bytes);
    for(auto [reg,dw]:r.inlines){word(b,uint32_t(reg));word(b,uint32_t(dw));}
    b.insert(b.end(),r.code.begin(),r.code.end()); return b;
}
template<class F> static void rejects(F f,const char* why) {
    bool rejected=false;try { f(); } catch(const std::exception&) {rejected=true;}
    require(rejected,why);
}
// Interpret only the emitted scalar instruction subset. The expected values
// below come from the root ABI, independently of prologue's moves/loads lists.
static void check_prologue(const Record& r) {
    Arguments a(r);auto b=prologue(r);
    require(b.size()%256==0,"unaligned prologue");
    std::array<uint32_t,128> s; s.fill(0xdeadbeef);
    const uint64_t spill=0x123400100000ull,grid=0x123400200000ull;
    const uint32_t table=0x300000,tgsize=0x76543210;
    auto push=[](unsigned i){return 0xabc00000u+7919*i;};
    std::map<uint64_t,uint32_t> mem;
    for(unsigned i=0;i<61;++i)mem[spill+16+4*i]=push(i);
    for(unsigned i=0;i<3;++i)mem[grid+4*i]=31+17*i;
    for(unsigned i=0;i<a.user;++i) {
        auto x=a.map[i];
        if(x==0x10000002)s[i]=uint32_t(spill);
        else if(x==0x10000006){s[i]=uint32_t(grid);s[i+1]=uint32_t(grid>>32);++i;}
        else if(x==65)s[i]=table;
        else if(x>=4 && x<=64)s[i]=push(unsigned(x-4));
    }
    if(r.rsrc2&(1<<10))s[a.user]=tgsize;
    bool waited=false,branched=false;
    for(size_t p=0;p<b.size();p+=4) {
        auto w=read<uint32_t>(b,p);unsigned dst=(w>>16)&127,src=w&255;
        if((w&0xff80ff00)==0xbe800000){require(src<128,"bad mov source");s[dst]=s[src];}
        else if((w&0xff80ff00)==0xbe804700){s[dst]=0x800;s[dst+1]=0x1234;}
        else if((w&0xff80ff00)==0xbe800f00)s[dst]=uint32_t(int32_t(int16_t(s[src])));
        else if((w&0xffff0000)==0xbf800000){}
        else if((w&0xffff0000)==0xbfa00000) {
            require(p+4+4*(w&65535)==b.size(),"branch does not reach ACO entry");branched=true;break;
        } else if(w==0xbfc70000)waited=true;
        else if((w&0xffffe000)==0xf4000000) {
            unsigned base=(w&63)*2,target=(w>>6)&127;
            auto literal=read<uint32_t>(b,p+4);require((literal&0xff000000)==0xf8000000,"bad SMEM encoding");
            uint64_t addr=(uint64_t(s[base+1])<<32)|s[base];addr+=literal&0xffffff;
            require(mem.count(addr),"SMEM used wrong pointer or offset");s[target]=mem.at(addr);p+=4;waited=false;
        } else if((w&0xff800000)==0x80000000) {
            require(src<128,"bad add source");s[dst]=s[src]+((w>>8)&255)-128;
        } else require(false,"unrecognized prologue instruction");
    }
    require(branched && waited,"missing entry branch or scalar load wait");
    if(r.set0>=0)require(s[r.set0]==table,"wrong descriptor register");
    if(r.push>=0)require(s[r.push]==uint32_t(spill)+16,"wrong push pointer");
    for(auto [reg,dw]:r.inlines)require(s[reg]==push(dw),"overwritten inline argument");
    if(r.grid>=0)for(unsigned j=0;j<3;++j)require(s[r.grid+j]==31+17*j,"wrong grid argument");
    if(r.rsrc2&(1<<10))require(s[r.user]==tgsize,"overwritten threadgroup size");
    require(s[125]==0x1234,"wrong address32 high word");
}
static void selftest() {
    unsigned cases=0;
    for(bool set:{false,true})for(bool pointer:{false,true})for(bool grid:{false,true})
    for(bool tg:{false,true})for(unsigned n=0;n<=24;++n) {
        Record r;r.mach=0x4e;r.push_bytes=244;r.wg=64;r.exec=4;word(r.code,0xbfb00000);
        unsigned next=0;if(set)r.set0=next++;if(pointer)r.push=next++;
        for(unsigned j=0;j<n;++j)r.inlines.emplace_back(next++,int((j*23)%61));
        if(grid){r.grid=next;next+=3;}r.user=next;
        r.rsrc2=(r.user<<1)|(tg?1<<10:0);
        auto parsed=Record::parse(serialize(r));check_prologue(parsed);++cases;
    }
    Record r;r.mach=0x4e;r.push_bytes=244;r.wg=64;r.exec=4;r.user=1;r.push=0;r.rsrc2=2;word(r.code,0xbfb00000);
    auto b=serialize(r);
    rejects([&]{auto x=b;x.back()^=1;Record::parse(x);},"accepted corrupted code");
    rejects([&]{auto x=b;x.pop_back();Record::parse(x);},"accepted truncated record");
    rejects([&]{auto x=b;write(x,56,int32_t(-2));Record::parse(x);},"accepted invalid sentinel");
    rejects([&]{auto x=b;write(x,48,uint32_t(243));Record::parse(x);},"accepted unaligned push block");
    rejects([&]{auto x=b;write(x,64,int32_t(0));Record::parse(x);},"accepted overlapping grid");
    rejects([&]{auto x=b;write(x,52,uint32_t(2));Record::parse(x);},"accepted resource/user mismatch");
    rejects([&]{auto x=b;write(x,84,uint32_t(1));Record::parse(x);},"accepted reserved field");
    rejects([&]{auto x=b;write(x,80,uint32_t(8));Record::parse(x);},"accepted scratch size without enable");
    rejects([&]{auto x=b;write(x,28,uint32_t(3));Record::parse(x);},"accepted scratch enable without size");
    r.inlines={{0,61}};r.push=-1;
    rejects([&]{Record::parse(serialize(r));},"accepted out-of-range push index");
    std::printf("[PASS] %u gfx1201 prologue simulations and 10 malformed-record checks\n",cases);
}

int main(int argc, char** argv) {
    try {
        selftest();
        for (int i = 1; i < argc; ++i) {
            auto r = onr::pal::Record::parse(onr::pal::load(argv[i]));
            onr::pal::Arguments a(r);
            auto p = onr::pal::prologue(r);
            onr::pal::require(p.size()%256 == 0, "unaligned prologue");
            std::printf("%s: user=%u push=%u set0=%d prologue=%zu code=%zu\n",
                        argv[i], a.user, r.push_bytes, r.set0, p.size(), r.code.size());
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
