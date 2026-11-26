// [device-fault] port-common module device-fault (KB FN-64; idea from GameNative wrapper-gamenative, GameNative/mesa wrapper-25
// src/vulkan/wrapper/wrapper_device.c: "VK_EXT_device_fault ... universally available on Mali and cheap when no fault occurs, so
// enable it by default whenever the base driver supports it"; this file is our own code, written for the dm/ core).
//
// VK_EXT_device_fault turns a bare VK_ERROR_DEVICE_LOST (and the GPU-hang fence fatals that follow a lost device) into a report from
// the driver: what kind of fault, the GPU virtual address it hit (with the driver's precision), vendor fault codes and their text.
// Before this, a Mali / PowerVR / Xclipse player's boot.log only had "VK_ERROR_DEVICE_LOST" and the crumbs (which pass), never why.
//
// Two calls:
//   front (vulkan_present.cpp, before vkCreateDevice): xbg_device_fault_enable(physical, available, dext, &draw.pNext)
//       asks for the extension and its deviceFault feature when the driver has both (any GPU: Mali drivers expose it since r38-ish,
//       Turnip / RADV / Xclipse on recent builds; the Qualcomm stock driver mostly not). DM_GFX_DEVICE_FAULT=0 leaves the device
//       exactly as before. Vendor binary dumps (deviceFaultVendorBinary) are never requested.
//   engine (xbg_fatalf / fence_fatal, any thread, once per process): xbg_device_fault_report(device, log, who)
//       vkGetDeviceFaultInfoEXT (count, then data) and one log line per address / vendor record, at most 16 of each.
// The spec only promises fault data after VK_ERROR_DEVICE_LOST; on other results the driver answers with zero counts, which is
// logged as such (cheap, no harm). Nothing here runs on the success path: the only cost of an enabled extension is the driver's own.
//
// Header-only, C++17 (inline variables: one copy in libdm.so for both translation units). No allocation in the report except two
// small std::vectors (the report runs in an engine fatal, not in a signal handler).
#pragma once
#include <vulkan/vulkan.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>

// port-common core/pc.h (the [pc] report line of the front); declared here so the report hunk is one line. A declaration only:
// nothing references pc_note unless the report hunk is synced (then pc-core comes with it).
extern "C" void pc_note(const char *id,int applied,const char *fmt,...);

// 1 = VK_EXT_device_fault and its deviceFault feature are enabled on the device the front created.
inline int dm_vk_device_fault = 0;
// Why it is (not) on, for the [pc] line and the boot log ("enabled", "driver has no VK_EXT_device_fault", ...).
inline const char *dm_vk_device_fault_why = "not asked (device created without the device-fault module)";
// The feature struct lives here, not on the front's stack frame of one block: it must outlive vkCreateDevice.
inline VkPhysicalDeviceFaultFeaturesEXT dm_vk_device_fault_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};

// DM_GFX_DEVICE_FAULT: unset / 1 = enable when the driver has it (default), 0 = never.
inline bool xbg_device_fault_wanted(){const char *e=getenv("DM_GFX_DEVICE_FAULT");return !(e && *e && atoi(e)==0);}

// Front side. `available` = the device's extension list (any container of VkExtensionProperties), `dext` = the names that go into
// VkDeviceCreateInfo (pushed to when enabled), `chain` = the pNext head of the features chain given to vkCreateDevice (e.g.
// &draw.pNext); the feature struct is linked in front of whatever is there. Returns true when enabled.
template<class Avail,class Names>
inline bool xbg_device_fault_enable(VkPhysicalDevice physical,const Avail &available,Names &dext,void **chain){
    dm_vk_device_fault=0;
    if(!xbg_device_fault_wanted()){dm_vk_device_fault_why="off (DM_GFX_DEVICE_FAULT=0)";return false;}
    bool ext=false;for(const auto &x:available)if(!strcmp(x.extensionName,VK_EXT_DEVICE_FAULT_EXTENSION_NAME))ext=true;
    if(!ext){dm_vk_device_fault_why="driver has no VK_EXT_device_fault";return false;}
    VkPhysicalDeviceFaultFeaturesEXT q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};f2.pNext=&q;vkGetPhysicalDeviceFeatures2(physical,&f2);
    if(!q.deviceFault){dm_vk_device_fault_why="VK_EXT_device_fault without the deviceFault feature";return false;}
    dm_vk_device_fault_features=VkPhysicalDeviceFaultFeaturesEXT{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    dm_vk_device_fault_features.deviceFault=VK_TRUE;dm_vk_device_fault_features.deviceFaultVendorBinary=VK_FALSE;
    dm_vk_device_fault_features.pNext=*chain;*chain=&dm_vk_device_fault_features;
    dext.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
    dm_vk_device_fault=1;dm_vk_device_fault_why="enabled (a lost device reports the faulting GPU address and vendor codes)";
    return true;
}

inline const char *xbg_device_fault_type_name(VkDeviceFaultAddressTypeEXT t){
    switch(t){
    case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT:return "none";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT:return "read-invalid";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT:return "write-invalid";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT:return "execute-invalid";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT:return "ip-unknown";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT:return "ip-invalid";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT:return "ip-fault";
    default:return "?";
    }
}

// The address range a record names: reportedAddress rounded down to its precision (a power of two, 0 or 1 = exact).
inline void xbg_device_fault_range(uint64_t addr,uint64_t precision,uint64_t *lo,uint64_t *hi){
    if(precision<=1){*lo=addr;*hi=addr+1;return;}
    if(precision&(precision-1)){*lo=addr;*hi=addr+precision;return;}   // not a power of two: the driver broke the rule; report as given
    *lo=addr&~(precision-1);*hi=*lo+precision;
}

// An engine FATAL text after which the driver may hold a fault report: a lost device (the word, or result -4 in the forms the
// engines print: "failed: -4", "= -4", "result=-4", "returned -4", "VkResult -4"), a fence failure or a GPU hang.
inline bool xbg_device_fault_text(const char *t){
    if(!t)return false;
    static const char *const words[]={"DEVICE_LOST","fence","Fence","GPU hang","hung"," -4",":-4","=-4","(-4)"};
    for(const char *w:words)if(strstr(t,w))return true;
    return false;
}

typedef void (*xbg_device_fault_log_fn)(const char *fmt,...);

// Engine side: once per process (later calls return at once). `who` = what failed ("fatal", "fence wait", ...).
// Returns the number of records logged (address + vendor), -1 when not enabled / not available.
inline int xbg_device_fault_report(VkDevice device,xbg_device_fault_log_fn log,const char *who){
    static std::atomic<int> once{0};
    if(once.exchange(1))return -1;
    if(!device){log("NVN device fault (%s): no device",who);return -1;}
    if(!dm_vk_device_fault){log("NVN device fault (%s): no report: %s",who,dm_vk_device_fault_why);return -1;}
    auto get=(PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(device,"vkGetDeviceFaultInfoEXT");
    if(!get){log("NVN device fault (%s): vkGetDeviceFaultInfoEXT not found",who);return -1;}
    VkDeviceFaultCountsEXT counts{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    VkResult r=get(device,&counts,nullptr);
    if(r!=VK_SUCCESS && r!=VK_INCOMPLETE){log("NVN device fault (%s): vkGetDeviceFaultInfoEXT(counts) = %d",who,int(r));return -1;}
    const uint32_t na=counts.addressInfoCount<16?counts.addressInfoCount:16,nv=counts.vendorInfoCount<16?counts.vendorInfoCount:16;
    const unsigned long long bin=(unsigned long long)counts.vendorBinarySize;
    std::vector<VkDeviceFaultAddressInfoEXT> ai(na?na:1);std::vector<VkDeviceFaultVendorInfoEXT> vi(nv?nv:1);
    VkDeviceFaultInfoEXT info{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};info.pAddressInfos=na?ai.data():nullptr;info.pVendorInfos=nv?vi.data():nullptr;info.pVendorBinaryData=nullptr;
    counts.addressInfoCount=na;counts.vendorInfoCount=nv;counts.vendorBinarySize=0;
    r=get(device,&counts,&info);
    if(r!=VK_SUCCESS && r!=VK_INCOMPLETE){log("NVN device fault (%s): vkGetDeviceFaultInfoEXT(info) = %d",who,int(r));return -1;}
    char desc[VK_MAX_DESCRIPTION_SIZE+1];memcpy(desc,info.description,VK_MAX_DESCRIPTION_SIZE);desc[VK_MAX_DESCRIPTION_SIZE]=0;
    log("NVN device fault (%s): \"%s\": %u address record(s), %u vendor record(s), vendor binary %llu bytes (not requested)%s",
        who,desc,counts.addressInfoCount,counts.vendorInfoCount,bin,r==VK_INCOMPLETE?" [incomplete]":"");
    for(uint32_t i=0;i<counts.addressInfoCount && i<na;i++){uint64_t lo,hi;xbg_device_fault_range(ai[i].reportedAddress,ai[i].addressPrecision,&lo,&hi);
        log("NVN device fault address %u: %s at 0x%llx (precision %llu: 0x%llx..0x%llx)",i,xbg_device_fault_type_name(ai[i].addressType),
            (unsigned long long)ai[i].reportedAddress,(unsigned long long)ai[i].addressPrecision,(unsigned long long)lo,(unsigned long long)hi);}
    for(uint32_t i=0;i<counts.vendorInfoCount && i<nv;i++){char d[VK_MAX_DESCRIPTION_SIZE+1];memcpy(d,vi[i].description,VK_MAX_DESCRIPTION_SIZE);d[VK_MAX_DESCRIPTION_SIZE]=0;
        log("NVN device fault vendor %u: \"%s\" code=0x%llx data=0x%llx",i,d,(unsigned long long)vi[i].vendorFaultCode,(unsigned long long)vi[i].vendorFaultData);}
    return int(counts.addressInfoCount+counts.vendorInfoCount);
}
