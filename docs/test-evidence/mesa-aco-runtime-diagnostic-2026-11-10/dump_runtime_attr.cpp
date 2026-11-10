#include "opennr_rr/vk.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
extern "C" void rr_vk_load();
int main(int argc,char**argv){try{
 if(argc!=3)return 2;
 rr_vk_load();onr::Device d;std::string err;if(!d.init(err))throw std::runtime_error(err);
 std::filesystem::create_directories(argv[2]);
 auto elf=d.pipeline_elf(argv[1],0);
 std::ofstream f(std::filesystem::path(argv[2])/"attribute.elf",std::ios::binary);f.write((char*)elf.data(),elf.size());
 auto k=d.load(argv[1],0);d.destroy(k);
 return elf.empty();
 }catch(const std::exception&e){std::cerr<<e.what();return 1;}}
