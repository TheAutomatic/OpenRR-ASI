#include "rr_pal_binary.hpp"
#include <iostream>
using namespace onr::pal;
void show(const Value& v) {
    if (v.kind == Value::UInt) std::cout << v.u;
    else if (v.kind == Value::String) std::cout << '"' << v.s << '"';
    else if (v.kind == Value::Array) { std::cout << '['; bool first=true; for (auto& x:v.items) {if(!first)std::cout<<',';first=false;show(x);} std::cout<<']'; }
    else if (v.kind == Value::Map) { std::cout << '{';bool first=true;for(auto& [k,x]:v.fields){if(!first)std::cout<<',';first=false;std::cout<<'"'<<k<<"\":";show(x);}std::cout<<'}'; }
    else if(v.raw==Bytes{0xc2})std::cout<<"false";else if(v.raw==Bytes{0xc3})std::cout<<"true";else std::cout<<"null";
}
int main(int argc,char**argv) {
    for(int i=1;i<argc;++i) {
        auto b=load(argv[i]);if(b.size()>16 && !memcmp(b.data(),"ONRELF1",7)) b.erase(b.begin(),b.begin()+16);
        Elf e(b); auto& n=e.data[e.find(".note")];size_t p=0;
        while(p<n.size()){auto ns=read<uint32_t>(n,p),ds=read<uint32_t>(n,p+4),type=read<uint32_t>(n,p+8);p+=12+((ns+3)&~3);if(type==32){std::cout<<argv[i]<<'\n';show(unpack(Bytes(n.begin()+p,n.begin()+p+ds)));std::cout<<'\n';}p+=(ds+3)&~3;}
    }
}
