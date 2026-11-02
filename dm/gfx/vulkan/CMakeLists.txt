cmake_minimum_required(VERSION 3.22)
project(dm_vulkan LANGUAGES C CXX)
set(E ${CMAKE_CURRENT_SOURCE_DIR}/engine)
add_library(dm_vulkan_engine STATIC
 ${E}/xbg_game.cpp ${E}/xbg_shader_key.c ${E}/xbg_command_clone.c
 ${E}/xbg_draw_plan.c ${E}/xbg_vertex.c ${E}/xbg_quad.c ${E}/xbg_stages.cpp
 ${E}/xbg_clear.cpp ${E}/xbg_blit.cpp ${E}/xbg_texture.c ${E}/xbg_depth.c
 ${E}/xbg_framebuffer.c ${E}/xbg_vulkan.cpp ${E}/xbg_nvn.cpp ${E}/xbg_sampler.cpp ${E}/xbg_spirv_quad.cpp ${E}/xbg_spirv_warp.cpp ${E}/xbg_spirv_lone.cpp ${E}/xbg_spirv_ldc.cpp ${E}/xbg_spirv_temps.cpp ${E}/xbg_spirv_raster.cpp ${E}/xbg_spirv_gspass.cpp ${E}/xbg_spirv_relaxed.cpp ${E}/xbg_spirv_peep.cpp)
target_include_directories(dm_vulkan_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ../nvn/include)
target_compile_features(dm_vulkan_engine PUBLIC cxx_std_17)
# [mt S4] -O3 (was -O2, which came after the toolchain Release -O3 and won). Android: also -mtune=cortex-x3 (the translation thread
# is pinned to the prime core: X3 on 8 Gen 2, X4 on 8 Gen 3). -mtune only changes instruction scheduling, never the ISA: the code
# stays plain armv8-a and runs on every arm64 Android device. No switch (pure codegen); docs/BUGS_AND_FIXES.md 3.20.
target_compile_options(dm_vulkan_engine PRIVATE -O3 -fno-strict-aliasing -Wall -Wextra -Wno-missing-field-initializers -Wno-format)
if(ANDROID)
  target_compile_options(dm_vulkan_engine PRIVATE -mtune=cortex-x3)
endif()
set_source_files_properties(${E}/xbg_texture.c PROPERTIES COMPILE_OPTIONS -fsigned-char)
set_target_properties(dm_vulkan_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(dm_vulkan_engine PUBLIC log nativewindow)   # Vulkan: trampolines in the Android host (vk_driver.c)
# nvn_vk.c is compiled in dmcore (host compatibility flags); engine deliberately
# reuses dmcore's nvn_tex.c / nvn_deswizzle.c rather than duplicate definitions.
