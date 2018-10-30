# Copyright 2018 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
if (NOT ("${ANDROID_TARGET_OS}" STREQUAL "windows_msvc"))
  prebuilt(X264)
endif ()
prebuilt(VPX)
get_filename_component(PREBUILT_ROOT "${ANDROID_QEMU2_TOP_DIR}/../../prebuilts/android-emulator-build/common/ffmpeg/${ANDROID_TARGET_TAG}" ABSOLUTE)

set(FFMPEG_INCLUDE_DIR "${PREBUILT_ROOT}/include/")
set(FFMPEG_INCLUDE_DIRS "${FFMPEG_INCLUDE_DIR}")
set(FFMPEG_LIBAVCODEC "${PREBUILT_ROOT}/lib/libavcodec${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(FFMPEG_LIBAVFORMAT "${PREBUILT_ROOT}/lib/libavformat${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(FFMPEG_LIBAVUTIL "${PREBUILT_ROOT}/lib/libavutil${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(FFMPEG_LIBRARIES "${PREBUILT_ROOT}/lib/libavformat${CMAKE_STATIC_LIBRARY_SUFFIX};${PREBUILT_ROOT}/lib/libavfilter${CMAKE_STATIC_LIBRARY_SUFFIX};${PREBUILT_ROOT}/lib/libavcodec${CMAKE_STATIC_LIBRARY_SUFFIX};${PREBUILT_ROOT}/lib/libswresample${CMAKE_STATIC_LIBRARY_SUFFIX};${PREBUILT_ROOT}/lib/libswscale${CMAKE_STATIC_LIBRARY_SUFFIX};${PREBUILT_ROOT}/lib/libavutil${CMAKE_STATIC_LIBRARY_SUFFIX};${X264_LIBRARIES};${VPX_LIBRARIES}")
set(FFMPEG_FOUND TRUE)
set(PACKAGE_EXPORT "FFMPEG_INCLUDE_DIR;FFMPEG_INCLUDE_DIRS;FFMPEG_LIBRARIES;FFMPEG_CLIENT_LIBRARIES;FFMPEG_FOUND;FFMPEG_LIBAVUTIL;FFMPEG_LIBAVFORMAT;FFMPEG_LIBAVCODEC")

