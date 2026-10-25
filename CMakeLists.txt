cmake_minimum_required(VERSION 3.21)
project(wxl-equip-plus VERSION 1.6.0 LANGUAGES NONE)

set(WXL_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk" CACHE PATH "Path to the WarcraftXL SDK")
include("${WXL_SDK_DIR}/cmake/wxl_add_mod.cmake")

# x64 only: wxl.m2 is published by the runtime the recompiled client loads, and only there.
wxl_add_mod(equip-plus
    SOURCES src/EquipPlus.cpp src/PhysicsLive.cpp src/physics/physics_domino.cpp
    MANIFEST mod.json
    OUTPUT_NAME wxl-equip-plus
    TARGETS win-x64)
