// Subgroup operations in a shader stage the device does not run them in (see xbg_spirv_lone.cpp)
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// Rewrites a module so that it uses no subgroup operation or subgroup built-in: every invocation acts as a subgroup of one
// lane (lane id 0, size 1). A ballot becomes uvec4(pred ? 1 : 0, 0, 0, 0), a shuffle / broadcast / reduction / scan returns
// the invocation's own value, all / any return the predicate, elect and all-equal return true. The GroupNonUniform*
// capabilities and the built-in variables are removed.
// Returns the number of instructions rewritten (0: out is empty and the module is to be used unchanged, including when it
// uses a subgroup feature this does not know).
size_t xbg_spirv_lone_lane(const uint32_t *words, size_t count, std::vector<uint32_t> &out);
