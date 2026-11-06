# Opus 盲测

一个使用 C++17 和 Win32 的 Windows 桌面应用。FFmpeg 和 libopus 静态链接到程序中。支持导入 MP3、AAC、FLAC、WAV、MP4 和 MKV，读取其中的第一条音轨。MP4 和 MKV 内的音轨需要使用已启用的编码，例如 AAC、ALAC、Opus、Vorbis、FLAC、AC-3、E-AC-3、DTS 或 TrueHD。

## 构建

以下命令使用 64 位 MinGW-w64、CMake、NASM、GNU Make 和 Git Bash。先将 FFmpeg n7.1.5、libopus 1.6.1 和 zlib 1.3.1 的静态库安装到同一个前缀。

在项目目录打开 Git Bash，假设 FFmpeg 源码在同级的 `FFmpeg-n7.1.5` 目录，libopus 和 zlib 已分别解压到 `build/deps/opus-1.6.1` 和 `build/deps/zlib-1.3.1`：

```bash
root=$(pwd -m)
prefix="$root/build/deps/prefix"
cmake -S build/deps/opus-1.6.1 -B build/deps/opus-build -G 'MinGW Makefiles' \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DOPUS_BUILD_SHARED_LIBRARY=OFF -DOPUS_PACKAGE_VERSION=1.6.1
cmake --build build/deps/opus-build --parallel
cmake --install build/deps/opus-build

cmake -S build/deps/zlib-1.3.1 -B build/deps/zlib-build -G 'MinGW Makefiles' \
  -DCMAKE_BUILD_TYPE=Release -DZLIB_BUILD_EXAMPLES=OFF
cmake --build build/deps/zlib-build --target zlibstatic --parallel
cp build/deps/zlib-build/libzlibstatic.a "$prefix/lib/libz.a"
cp build/deps/zlib-1.3.1/zlib.h build/deps/zlib-build/zconf.h "$prefix/include/"

mkdir -p build/deps/ffmpeg
test -f build/deps/ffmpeg/src/configure || cp -R ../FFmpeg-n7.1.5 build/deps/ffmpeg/src
(
  cd build/deps/ffmpeg
  sh src/configure --prefix="$prefix" --target-os=mingw32 \
    --extra-cflags="-I$prefix/include" --extra-ldflags="-L$prefix/lib" \
    --arch=x86_64 --disable-autodetect --disable-everything --disable-programs \
    --disable-doc --disable-network --disable-avdevice --disable-avfilter \
    --disable-swscale --disable-postproc --disable-shared --enable-static \
    --enable-small --enable-zlib --enable-protocol=file \
    --enable-demuxer=mp3,aac,flac,wav,mov,matroska,ogg \
    --enable-parser=mpegaudio,aac,aac_latm,flac,ac3,dca,mlp,opus,vorbis \
    '--enable-decoder=mp3,mp3float,aac,aac_latm,flac,alac,opus,vorbis,ac3,eac3,dca,truehd,pcm_*' &&
  gmake -j8 SHELL=sh.exe && gmake install SHELL=sh.exe
)
```

依赖安装完成后，在 PowerShell 中构建应用。若使用其他安装目录，配置时传入 `-DMEDIA_ROOT=...`。

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
.\build\OpusBlindTest.exe
```

MinGW-w64 默认构建会生成两份 EXE：`OpusBlindTest.exe` 保留符号表和符号名称，`OpusBlindTest-stripped.exe` 移除符号表和调试信息，适合分发。

## 测试

1. 将一个音频或视频文件拖入窗口，或点击「打开媒体」。应用读取第一条音轨的前 90 秒，转换为 48 kHz 双声道，留出初始音量余量。每题从中随机抽取一个最长 15 秒的片段，相邻两题使用不同片段。短于 10 秒的音轨使用完整音频。
2. 播放 A 和 B。两段均由同一个片段编码，一段固定为 256 kbps Opus 参照，另一段使用当前档位的 Opus 码率。每题随机交换顺序，并在同一时间开放播放。试听时，A 和 B 共用同一条采样时间线，切换会从下一块尚未提交的音频开始，经过约 10 毫秒的交叉淡化。重复点击当前版本会继续播放。开始试听后，程序会在后台编码下一题可能用到的版本。
3. 选择你认为音质更好的一段。选中对比版本时会提示「挑战失败」并显示该版本的码率，清零连对次数，然后降一档。已经处于最低档时留在该档。开始播放下一题时，失败提示会消失。

对比码率依次为 **20、32、64、96、128、192 kbps**，从 **32 kbps** 开始，两段均使用 Opus 恒定码率。同一档连续 3 次选中参照版本会立即升档，进入下一档后重新计数。最高档连续 12 次选中参照版本后完成测试，结果页显示最后一题 A、B 各自的码率和选中参照版本的总次数。
