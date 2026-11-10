#include "../src/optifg_provider_locator.hpp"
int main(int argc,char**argv){
 if(argc!=2)return 2;
 auto m=LoadLibraryExA(argv[1],nullptr,DONT_RESOLVE_DLL_REFERENCES);
 if(!m)return 2;
 auto p=optifg_probe::locate(m);
 std::puts(p ? "AUTO_LOCATED patched=false" : "REJECT");
 FreeLibrary(m);return p?0:1;
}
