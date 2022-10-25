//===- llvm-omp-device-info.cpp - Obtain device info as seen from OpenMP --===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This is a command line utility that, by using Libomptarget, and the device
// plugins, list devices information as seen from the OpenMP Runtime.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include <iostream>
#include<fstream>
#include <sstream>

#include "omptarget.h"
#include <cstdio>

int main(int argc, char **argv) {

  std::string json_filename = std::string(argv[1]);
  std::cout << "Reading " << json_filename << std::endl;

  std::string replay_directory = "./";
  const size_t last_slash_idx = json_filename.rfind('/');
  if (std::string::npos != last_slash_idx)
  {
    replay_directory = json_filename.substr(0, last_slash_idx);
  }
  std::cout << "Replay directory is " << replay_directory << std::endl;

  std::ifstream json_file(json_filename);
  std::stringstream buffer;
  buffer << json_file.rdbuf();
  json_file.close();
  std::string JsonStr = buffer.str();

  llvm::StringRef JsonStrRef(JsonStr);
  llvm::Expected<llvm::json::Value> Exp = llvm::json::parse(JsonStrRef);
  if (auto Err = Exp.takeError())
    return 1;

  __tgt_bin_desc BinDesc = {0, nullptr, nullptr, nullptr}; 

  // (1)
  // __tgt_bin_desc int32_t NumDeviceImages
  //
  llvm::Optional<int64_t> NumDeviceImages = Exp->getAsObject()->getInteger("NumDeviceImages");
  BinDesc.NumDeviceImages = NumDeviceImages.getValue();

  // (2)
  // __tgt_bin_desc __tgt_device_image *DeviceImages
  //
  std::vector<__tgt_device_image> DeviceImages;
  std::vector<__tgt_offload_entry> HostEntries;
  std::vector<std::vector<char> > device_images;
  std::vector<std::string> EntryNames;
  llvm::json::Array *DeviceImagesJson = Exp->getAsObject()->getArray("DeviceImages");
  for (llvm::json::Array::iterator I = DeviceImagesJson->begin(); I != DeviceImagesJson->end(); ++I)
  {
    __tgt_device_image DeviceImage = {nullptr, nullptr, nullptr, nullptr};
    llvm::Optional<llvm::StringRef> device_image_filename =  I->getAsObject()->getString("ImageFile");
    std::cout << "Found device image " << device_image_filename.getValue().str() << "!\n";
    // read binary image
    std::ifstream device_image_file;
    device_image_file.open(std::string(replay_directory + '/' + device_image_filename.getValue().str()), std::ios::binary | std::ios::in);
    device_image_file.seekg(0, device_image_file.end);
    size_t length = device_image_file.tellg();
    std::cout << "Length:" << length << "\n";
    device_image_file.seekg (0, device_image_file.beg);
    device_images.push_back(std::vector<char>(length));
    device_image_file.read(&(device_images.back()[0]), length);
    device_image_file.close();
    // end read binary image
    llvm::Optional<int64_t> ImageStart = I->getAsObject()->getInteger("ImageStart");
    DeviceImage.ImageStart = (void*)ImageStart.getValue();
    llvm::Optional<int64_t> ImageEnd = I->getAsObject()->getInteger("ImageEnd");
    DeviceImage.ImageEnd = (void*)ImageEnd.getValue();
    assert((char*)DeviceImage.ImageEnd - (char*)DeviceImage.ImageStart == length);
    llvm::json::Array *DeviceImageEntriesJson = I->getAsObject()->getArray("Entries");
    DeviceImage.EntriesBegin = new __tgt_offload_entry[DeviceImageEntriesJson->size()];
    DeviceImage.EntriesEnd = DeviceImage.EntriesBegin;
    for (llvm::json::Array::iterator II = DeviceImageEntriesJson->begin(); II != DeviceImageEntriesJson->end(); ++II)
    {
      __tgt_offload_entry *Entry = DeviceImage.EntriesEnd;
      llvm::Optional<int64_t> addr = II->getAsObject()->getInteger("addr");
      Entry->addr = (void*)addr.getValue();
      ptrdiff_t offset = (char*)Entry->addr - (char*)DeviceImage.ImageStart;  // recalculate
      Entry->addr = &(device_images.back()[0]) + offset;                      // the device function address 
      llvm::Optional<llvm::StringRef> name = II->getAsObject()->getString("name");
      EntryNames.push_back(name.getValue().str());
      Entry->name = const_cast<char*>(EntryNames.back().c_str());
      llvm::Optional<int64_t> size = II->getAsObject()->getInteger("size");
      Entry->size = (size_t)size.getValue();
      llvm::Optional<int64_t> flags = II->getAsObject()->getInteger("flags");
      Entry->flags = (int32_t)flags.getValue();
      llvm::Optional<int64_t> reserved = II->getAsObject()->getInteger("reserved");
      Entry->reserved = (int32_t)reserved.getValue();
      ++DeviceImage.EntriesEnd;
      HostEntries.push_back(*Entry);  // just make a copy of the device entry in the host entries
    }
    DeviceImage.ImageStart = &(device_images.back()[0]);
    DeviceImage.ImageEnd = (void*)((char*)DeviceImage.ImageStart + length);
    DeviceImages.push_back(DeviceImage);
  }
  BinDesc.DeviceImages = DeviceImages.data();

  // (3) -- NOTE: use a copy of device entries instead 
  // __tgt_offload_entry *HostEntriesBegin
  // __tgt_offload_entry *HostEntriesEnd
  //std::vector<__tgt_offload_entry> HostEntries;
  //llvm::json::Array *HostEntriesJson = Exp->getAsObject()->getArray("HostEntries");
  //for (llvm::json::Array::iterator I = HostEntriesJson->begin(); I != HostEntriesJson->end(); ++I)
  //{
  //  __tgt_offload_entry Entry = {nullptr, nullptr, 0, 0, 0}; 
  //  llvm::Optional<int64_t> addr = I->getAsObject()->getInteger("addr");
  //  Entry.addr = (void*)addr.getValue();
  //  llvm::Optional<llvm::StringRef> name = I->getAsObject()->getString("name");
  //  std::cout << "Found name " << name.getValue().str() << "!\n";
  //  Entry.name = new char[name.getValue().str().size()];
  //  name.getValue().str().copy(Entry.name, name.getValue().str().size());
  //  llvm::Optional<int64_t> size = I->getAsObject()->getInteger("size");
  //  Entry.size = (size_t)size.getValue();
  //  llvm::Optional<int64_t> flags = I->getAsObject()->getInteger("flags");
  //  Entry.flags = (int32_t)flags.getValue();
  //  llvm::Optional<int64_t> reserved = I->getAsObject()->getInteger("reserved");
  //  Entry.reserved = (int32_t)reserved.getValue();
  //  HostEntries.push_back(Entry);
  //}
  BinDesc.HostEntriesBegin = HostEntries.data();
  BinDesc.HostEntriesEnd = HostEntries.data();
  BinDesc.HostEntriesEnd += HostEntries.size();

  // print out
  std::cout << "NumDeviceImages:" << NumDeviceImages.getValue() << std::endl;
  //std::cout << "BinDesc.DeviceImages:" << BinDesc.DeviceImages << std::endl; 
  std::cout << "BinDesc.DeviceImage->ImageStart:" << BinDesc.DeviceImages->ImageStart << std::endl;
  std::cout << "BinDesc.DeviceImage->ImageEnd:" << BinDesc.DeviceImages->ImageEnd << std::endl;
  for (__tgt_offload_entry *I = BinDesc.DeviceImages->EntriesBegin; I != BinDesc.DeviceImages->EntriesEnd; ++I)
  {
    std::cout << "BinDesc.DeviceImage.Entry.addr:" << I->addr << std::endl;
    std::cout << "BinDesc.DeviceImage.Entry.name:" << I->name << std::endl;
    std::cout << "BinDesc.DeviceImage.Entry.size:" << I->size << std::endl;
    std::cout << "BinDesc.DeviceImage.Entry.flags:" << I->flags << std::endl;
    std::cout << "BinDesc.DeviceImage.Entry.reserved:" << I->reserved << std::endl;
  }
  for (__tgt_offload_entry *I = BinDesc.HostEntriesBegin; I != BinDesc.HostEntriesEnd; ++I)
  {
    std::cout << "BinDesc.HostEntry.addr:" << I->addr << std::endl;
    std::cout << "BinDesc.HostEntry.name:" << I->name << std::endl;
    std::cout << "BinDesc.HostEntry.size:" << I->size << std::endl;
    std::cout << "BinDesc.HostEntry.flags:" << I->flags << std::endl;
    std::cout << "BinDesc.HostEntry.reserved:" << I->reserved << std::endl;
  }

  __tgt_register_lib(&BinDesc);

//  __tgt_bin_desc EmptyDesc = {0, nullptr, nullptr, nullptr};
// IN FILE "include/omptarget.h"
//130 /// This struct is a record of all the host code that may be offloaded to a
//131 /// target.
//132 struct __tgt_bin_desc {
//133   int32_t NumDeviceImages;           // Number of device types supported
//134   __tgt_device_image *DeviceImages;  // Array of device images (1 per dev. type)
//135   __tgt_offload_entry *HostEntriesBegin; // Begin of table with all host entries
//136   __tgt_offload_entry *HostEntriesEnd;   // End of table (non inclusive)
//137 };

//  __tgt_register_lib(&EmptyDesc);
// IN FILE include/omptarget.h
//234 /// adds a target shared library to the target execution image
//235 void __tgt_register_lib(__tgt_bin_desc *desc);

// IN FILE src/interface.cpp
// 31 ////////////////////////////////////////////////////////////////////////////////
// 32 /// adds a target shared library to the target execution image
// 33 EXTERN void __tgt_register_lib(__tgt_bin_desc *desc) {
// 34   TIMESCOPE();
// 35   std::call_once(PM->RTLs.initFlag, &RTLsTy::LoadRTLs, &PM->RTLs);
// 36   for (auto &RTL : PM->RTLs.AllRTLs) {
// 37     if (RTL.register_lib) {
// 38       if ((*RTL.register_lib)(desc) != OFFLOAD_SUCCESS) {
// 39         DP("Could not register library with %s", RTL.RTLName.c_str());
// 40       }
// 41     }
// 42   }
// 43   PM->RTLs.RegisterLib(desc);
// 44 }


// TODO this might need to change
  __tgt_init_all_rtls();
// IN FILE src/interface.cpp
// 47 /// Initialize all available devices without registering any image
// 48 EXTERN void __tgt_init_all_rtls() { PM->RTLs.initAllRTLs(); }


  //for (int Dev = 0; Dev < omp_get_num_devices(); Dev++) {
  //  printf("Device (%d):\n", Dev);
  //  if (!__tgt_print_device_info(Dev))
  //    printf("    print_device_info not implemented\n");
  //  printf("\n");
  //}

// TRANSFER BUMP ALLOCATOR DATA TO GPU
// use return base pointer to update new device pointers


// THIS ALSO TRANSFERS CONTROL TO DEVICE AND EXECUTES
// and calls __tgt_target_teams_mapper
//
//int __tgt_target_teams(int64_t device_id, void *host_ptr, int32_t arg_num,
//                       void **args_base, void **args, int64_t *arg_sizes,
//                       int64_t *arg_types, int32_t num_teams,
//                       int32_t thread_limit);
//__tgt_target_teams(0, host_ptr, -1, 
//                   nullptr, nullptr, nullptr,
//                   nullptr, num_teams, 
//                   thread_limit);

std::string kernel_filename_json = replay_directory + '/' + std::string(HostEntries[0].name);
char *kernel_filename_json_ptr = const_cast<char*>(kernel_filename_json.c_str());

int64_t magic = 0x5245504C41594D45; //Ascii "REPLAYME"
__tgt_target_teams(0, HostEntries[0].addr, 0,
                   nullptr, (void**)(&kernel_filename_json_ptr), &magic,
                   //nullptr, (void**)(&(HostEntries[0].name)), &magic,
                   nullptr, 0,
                   0);



//  __tgt_unregister_lib(&EmptyDesc);
  __tgt_unregister_lib(&BinDesc);
  

  for (int i=0; i<DeviceImages.size(); ++i)
  {
    delete DeviceImages[i].EntriesBegin;
  }

  return 0;
}
