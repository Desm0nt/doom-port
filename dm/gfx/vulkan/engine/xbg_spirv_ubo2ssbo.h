// Dynamically indexed uniform blocks read as read-only storage buffers (see xbg_spirv_ubo2ssbo.cpp)
#pragma once
#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

// Every Uniform-class Block variable (set/binding decorated, not an array of blocks) that some access chain indexes with a
// non-constant index becomes a StorageBuffer variable of a copy of its block type, decorated NonWritable (variable and every
// member), with Binding / DescriptorSet / Offset / ArrayStride unchanged (DOOM's blocks are vec4 arrays of stride 16, the same
// under std140 and std430). Access chains rooted at it get StorageBuffer pointer types. SPIR-V < 1.3 gets the
// SPV_KHR_storage_buffer_storage_class extension. A variable used in any other way than access chains and loads (function
// arguments, copies, phis, ...) stays a uniform block.
// allowed: only these bindings are considered (nullptr: all). converted: the bindings rewritten, ascending.
// dry_run: only report the bindings that would be rewritten; words unchanged.
// Returns the number of variables rewritten (or that would be).
size_t xbg_spirv_ubo2ssbo(std::vector<uint32_t> &words, const std::set<uint32_t> *allowed, std::vector<uint32_t> *converted,
                          bool dry_run = false);
