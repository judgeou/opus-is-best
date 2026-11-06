cmake_minimum_required(VERSION 3.20)
project(OpusBlindTest LANGUAGES CXX)

set(MEDIA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/build/deps/prefix" CACHE PATH
    "Install prefix for static FFmpeg, libopus and zlib")

find_path(MEDIA_INCLUDE_DIR libavformat/avformat.h PATHS "${MEDIA_ROOT}/include"
    NO_DEFAULT_PATH REQUIRED)
foreach(library IN ITEMS avformat avcodec swresample avutil opus)
    if(MINGW)
        set(archive "lib${library}.a")
    else()
        set(archive "${library}.lib")
    endif()
    find_file(MEDIA_${library} NAMES "${archive}" PATHS "${MEDIA_ROOT}/lib"
        NO_DEFAULT_PATH REQUIRED)
    list(APPEND media_libraries "${MEDIA_${library}}")
endforeach()
if(MINGW)
    set(zlib_archive libz.a)
else()
    set(zlib_archive zlibstatic.lib)
endif()
find_file(MEDIA_zlib NAMES "${zlib_archive}" PATHS "${MEDIA_ROOT}/lib"
    NO_DEFAULT_PATH REQUIRED)

add_executable(OpusBlindTest WIN32 src/main.cpp src/audio.cpp)
target_compile_features(OpusBlindTest PRIVATE cxx_std_17)
target_compile_definitions(OpusBlindTest PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(OpusBlindTest SYSTEM PRIVATE "${MEDIA_INCLUDE_DIR}")
target_link_libraries(OpusBlindTest PRIVATE ${media_libraries} "${MEDIA_zlib}"
    ole32 comdlg32 shell32 comctl32 bcrypt)
if(MSVC)
    target_compile_options(OpusBlindTest PRIVATE /utf-8 /W4)
elseif(MINGW)
    enable_language(RC)
    target_sources(OpusBlindTest PRIVATE src/app.rc)
    target_compile_options(OpusBlindTest PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(OpusBlindTest PRIVATE -municode -static)

    get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    set(stripped_exe "${CMAKE_CURRENT_BINARY_DIR}/OpusBlindTest-stripped.exe")
    if(multi_config)
        set(stripped_exe "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/OpusBlindTest-stripped.exe")
    endif()
    add_custom_command(
        OUTPUT "${stripped_exe}"
        COMMAND "${CMAKE_STRIP}" --strip-all -o "${stripped_exe}" "$<TARGET_FILE:OpusBlindTest>"
        DEPENDS OpusBlindTest
        VERBATIM
    )
    add_custom_target(OpusBlindTestStripped ALL DEPENDS "${stripped_exe}")
endif()
