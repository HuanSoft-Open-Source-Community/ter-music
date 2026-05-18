cmake_minimum_required(VERSION 3.10)
project(ter-music LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 静态链接选项（仅将 FFmpeg 库静态链接，其他库动态链接）
# FFmpeg 的 soname 在不同 EL 版本间不同，是跨版本兼容的主要障碍。
# 其他库（ncurses、libcurl、libpng、libjpeg、pulse）的 soname
# 在 EL8/9/10 间稳定，使用动态链接即可。
option(STATIC_LINKING "Statically link FFmpeg only, other libs dynamic" OFF)

if(STATIC_LINKING)
    message(STATUS "静态链接模式已启用（仅 FFmpeg 静态）")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc")
endif()

# 查找必要的库
find_package(PkgConfig REQUIRED)
if(NOT PKG_CONFIG_FOUND)
    message(FATAL_ERROR "pkg-config is required but not found.")
endif()

# FFmpeg 库（静态链接时使用 --static 获取所有传递依赖，并优先查找 .a 文件）
if(STATIC_LINKING)
    set(ENV{PKG_CONFIG_ARGN} "--static")
    set(CMAKE_FIND_LIBRARY_SUFFIXES .a ${CMAKE_FIND_LIBRARY_SUFFIXES})
endif()
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(AVFILTER REQUIRED libavfilter)
pkg_check_modules(SWSCALE REQUIRED libswscale)
if(STATIC_LINKING)
    unset(ENV{PKG_CONFIG_ARGN})
endif()
pkg_check_modules(PNG REQUIRED libpng)
pkg_check_modules(JPEG REQUIRED libjpeg)
pkg_check_modules(PULSE libpulse)
pkg_check_modules(DBUS dbus-1)
pkg_check_modules(LIBCURL libcurl)

if(PULSE_FOUND)
    set(AUDIO_BACKEND_NAME "PulseAudio")
    set(AUDIO_BACKEND_INCLUDE_DIRS ${PULSE_INCLUDE_DIRS})
    set(AUDIO_BACKEND_LIBRARIES ${PULSE_LIBRARIES})
    set(AUDIO_BACKEND_CFLAGS ${PULSE_CFLAGS_OTHER})
    set(AUDIO_BACKEND_DEFINES HAVE_PULSE=1)
else()
    pkg_check_modules(ALSA REQUIRED alsa)
    set(AUDIO_BACKEND_NAME "ALSA")
    set(AUDIO_BACKEND_INCLUDE_DIRS ${ALSA_INCLUDE_DIRS})
    set(AUDIO_BACKEND_LIBRARIES ${ALSA_LIBRARIES})
    set(AUDIO_BACKEND_CFLAGS ${ALSA_CFLAGS_OTHER})
    set(AUDIO_BACKEND_DEFINES HAVE_ALSA=1)
endif()

message(STATUS "Using audio backend: ${AUDIO_BACKEND_NAME}")
if(DBUS_FOUND)
    message(STATUS "Using system media session backend: MPRIS (D-Bus)")
else()
    message(STATUS "System media session backend: disabled (dbus-1 not found)")
endif()

# 查找ncurses库（优先ncursesw，回退到ncurses）
find_library(NCURSESW_LIBRARY NAMES ncursesw ncurses)
if(NOT NCURSESW_LIBRARY)
    message(FATAL_ERROR "ncurses library not found")
endif()

# 查找pthread库
find_library(PTHREAD_LIBRARY pthread)
if(NOT PTHREAD_LIBRARY)
    message(FATAL_ERROR "pthread library not found")
endif()

# 添加可执行文件
add_executable(${PROJECT_NAME}
    src/main.c
    src/ui.c
    src/playlist.c
    src/audio.c
    src/progress.c
    src/lyrics.c
    src/media_session.c
    src/menu_views.c
    src/crypto.c
    src/braille_art.c
    src/image_loader.c
    src/remote.c
    src/logger.c
)

# 包含头文件目录
target_include_directories(${PROJECT_NAME} PRIVATE 
    include 
    ${AVCODEC_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${AVFILTER_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${AUDIO_BACKEND_INCLUDE_DIRS}
    ${PNG_INCLUDE_DIRS}
    ${JPEG_INCLUDE_DIRS}
)

if(DBUS_FOUND)
    target_include_directories(${PROJECT_NAME} PRIVATE ${DBUS_INCLUDE_DIRS})
endif()

if(LIBCURL_FOUND)
    message(STATUS "Using libcurl: ${LIBCURL_VERSION}")
    target_include_directories(${PROJECT_NAME} PRIVATE ${LIBCURL_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} ${LIBCURL_LIBRARIES})
    target_compile_options(${PROJECT_NAME} PRIVATE ${LIBCURL_CFLAGS_OTHER})
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_LIBCURL=1)
else()
    message(FATAL_ERROR "libcurl is required for remote playback (SMB/SFTP/FTP/WebDAV)")
endif()

# 链接库
target_link_libraries(${PROJECT_NAME}
    ${AVCODEC_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${AVFILTER_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    ${AUDIO_BACKEND_LIBRARIES}
    ${PNG_LIBRARIES}
    ${JPEG_LIBRARIES}
    ${NCURSESW_LIBRARY}
    m
    ${PTHREAD_LIBRARY}
)

if(DBUS_FOUND)
    target_link_libraries(${PROJECT_NAME} ${DBUS_LIBRARIES})
endif()

# 添加编译选项
target_compile_options(${PROJECT_NAME} PRIVATE
    ${AVCODEC_CFLAGS_OTHER}
    ${AVFORMAT_CFLAGS_OTHER}
    ${SWRESAMPLE_CFLAGS_OTHER}
    ${AVUTIL_CFLAGS_OTHER}
    ${AVFILTER_CFLAGS_OTHER}
    ${SWSCALE_CFLAGS_OTHER}
    ${AUDIO_BACKEND_CFLAGS}
    ${PNG_CFLAGS_OTHER}
    ${JPEG_CFLAGS_OTHER}
)

if(DBUS_FOUND)
    target_compile_options(${PROJECT_NAME} PRIVATE ${DBUS_CFLAGS_OTHER})
endif()

if(AVFILTER_FOUND)
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_AVFILTER=1)
endif()

# 添加预处理器定义
target_compile_definitions(${PROJECT_NAME} PRIVATE
    _GNU_SOURCE
    ${AUDIO_BACKEND_DEFINES}
)

if(DBUS_FOUND)
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_DBUS=1)
endif()

# 安装规则
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

# 安装 desktop 文件
install(FILES data/applications/${PROJECT_NAME}.desktop
    DESTINATION share/applications
)

# 安装图标
install(FILES resources/icons/hicolor/32x32/apps/${PROJECT_NAME}.png
    DESTINATION share/icons/hicolor/32x32/apps
)
install(FILES resources/icons/hicolor/48x48/apps/${PROJECT_NAME}.png
    DESTINATION share/icons/hicolor/48x48/apps
)
install(FILES resources/icons/hicolor/128x128/apps/${PROJECT_NAME}.png
    DESTINATION share/icons/hicolor/128x128/apps
)
install(FILES resources/icons/hicolor/scalable/apps/${PROJECT_NAME}.svg
    DESTINATION share/icons/hicolor/scalable/apps
)
