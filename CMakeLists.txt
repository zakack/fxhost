cmake_minimum_required(VERSION 3.22)
project(fxhost VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_subdirectory(JUCE EXCLUDE_FROM_ALL)

find_package(PkgConfig REQUIRED)
pkg_check_modules(JACK REQUIRED IMPORTED_TARGET jack)

juce_add_gui_app(fxhost
  PRODUCT_NAME "fxhost"
  COMPANY_NAME "fxhost")

target_sources(fxhost PRIVATE Source/Main.cpp)

target_compile_definitions(fxhost PRIVATE
  JUCE_PLUGINHOST_VST3=1
  JUCE_PLUGINHOST_VST=0
  JUCE_PLUGINHOST_LV2=0
  JUCE_PLUGINHOST_LADSPA=0
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0)

target_link_libraries(fxhost PRIVATE
  juce::juce_audio_processors
  juce::juce_gui_basics
  PkgConfig::JACK
  juce::juce_recommended_config_flags
  juce::juce_recommended_warning_flags)
