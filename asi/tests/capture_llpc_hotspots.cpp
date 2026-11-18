// SPDX-License-Identifier: MIT
// Compile-only Windows driver capture; no dispatch or performance data.
#include "opennr_rr/vk.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
extern "C" void rr_vk_load(void);
int main(int argc,char** argv) {
 try {
  if(argc!=4)throw std::runtime_error("capture_llpc_hotspots shader-dir list.txt output-dir");
  rr_vk_load();
  onr::Device d;onr::DeviceOptions opt{};opt.descriptors=true;std::string err;
  if(!d.init(err,opt))throw std::runtime_error(err);
  const auto out=onr::u8_to_path(argv[3]);std::filesystem::create_directories(out);
  std::ofstream info(out/"device.txt");
  info<<d.info().name<<std::endl<<d.info().driver_info<<std::endl<<d.info().driver_version<<std::endl;
  std::ifstream list(onr::u8_to_path(argv[2]));std::string name;
  while(std::getline(list,name)) {
   if(!name.empty() && name.back()==char(13))name.pop_back();if(name.empty())continue;
   const auto path=onr::u8_to_path(argv[1])/onr::u8_to_path(name);
   auto elf=d.pipeline_elf(onr::path_to_u8(path),32);
   if(elf.empty())throw std::runtime_error("Missing ELF: "+name);
   std::ofstream f(out/onr::u8_to_path(name+".elf"),std::ios::binary);
   f.write(reinterpret_cast<const char*>(elf.data()),elf.size());
   if(!f)throw std::runtime_error("Cannot save ELF: "+name);
   std::cout<<name<<' '<<elf.size()<<std::endl;
  }
 } catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}
}
