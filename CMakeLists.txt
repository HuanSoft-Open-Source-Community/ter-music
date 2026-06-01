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
    # Save original suffixes and prefer .a only for the FFmpeg pkg_check_modules block
    set(_saved_find_library_suffixes "${CMAKE_FIND_LIBRARY_SUFFIXES}")
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
    set(CMAKE_FIND_LIBRARY_SUFFIXES "${_saved_find_library_suffixes}")
    unset(_saved_find_library_suffixes)
endif()
pkg_check_modules(PNG REQUIRED libpng)
pkg_check_modules(JPEG REQUIRED libjpeg)
# PulseAudio 和 ALSA 仅用于获取头文件路径，运行时通过 dlopen 加载
pkg_check_modules(PULSE libpulse)
pkg_check_modules(ALSA alsa)
pkg_check_modules(DBUS dbus-1)
pkg_check_modules(LIBCURL libcurl)
pkg_check_modules(LIBXML2 REQUIRED libxml-2.0)
# SQLite3 — required dependency
# Install libsqlite3-dev (Debian) or sqlite-devel (RHEL) before building
pkg_check_modules(SQLITE3 REQUIRED sqlite3)
message(STATUS "Found SQLite3: ${SQLITE3_LIBRARIES} (pkg-config)")

if(PULSE_FOUND)
    set(AUDIO_BACKEND_INCLUDE_DIRS ${PULSE_INCLUDE_DIRS})
    set(AUDIO_BACKEND_CFLAGS ${PULSE_CFLAGS_OTHER})
endif()
if(ALSA_FOUND)
    set(AUDIO_BACKEND_INCLUDE_DIRS ${AUDIO_BACKEND_INCLUDE_DIRS} ${ALSA_INCLUDE_DIRS})
    set(AUDIO_BACKEND_CFLAGS ${AUDIO_BACKEND_CFLAGS} ${ALSA_CFLAGS_OTHER})
endif()

if(PULSE_FOUND AND ALSA_FOUND)
    message(STATUS "Audio backend(s): PulseAudio + ALSA (runtime dlopen)")
elseif(PULSE_FOUND)
    message(STATUS "Audio backend(s): PulseAudio (runtime dlopen)")
elseif(ALSA_FOUND)
    message(STATUS "Audio backend(s): ALSA (runtime dlopen)")
else()
    message(STATUS "Audio backend(s): NONE (will probe at runtime)")
endif()
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
    src/search.c
    src/scrollbar.c
    src/ape_tag.c
    src/config_xml.c
    src/config_migration.c
    src/library.c
    src/library_browser.c
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
    ${LIBXML2_INCLUDE_DIRS}
    ${SQLITE3_INCLUDE_DIRS}
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
    ${PNG_LIBRARIES}
    ${JPEG_LIBRARIES}
    ${NCURSESW_LIBRARY}
    ${LIBXML2_LIBRARIES}
    ${SQLITE3_LIBRARIES}
    m
    ${PTHREAD_LIBRARY}
    ${CMAKE_DL_LIBS}
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
    ${LIBXML2_CFLAGS_OTHER}
    ${SQLITE3_CFLAGS_OTHER}
)

if(DBUS_FOUND)
    target_compile_options(${PROJECT_NAME} PRIVATE ${DBUS_CFLAGS_OTHER})
endif()

if(AVFILTER_FOUND)
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_AVFILTER=1)
endif()

# 添加预处理器定义
set(TER_MUSIC_DATA_DIR "${CMAKE_INSTALL_PREFIX}/share/${PROJECT_NAME}")
target_compile_definitions(${PROJECT_NAME} PRIVATE
    _GNU_SOURCE
    TER_MUSIC_DATA_DIR="${TER_MUSIC_DATA_DIR}"
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

# 安装帮助文档
install(FILES data/help-quickstart-zh.txt data/help-quickstart-en.txt
        DESTINATION share/ter-music
)

# Uninstall target — removes files listed in install_manifest.txt
add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -E echo "Uninstalling ${PROJECT_NAME}..."
    COMMAND sh -c "if [ -f '${CMAKE_BINARY_DIR}/install_manifest.txt' ]; then xargs rm -f < '${CMAKE_BINARY_DIR}/install_manifest.txt'; else echo 'No install manifest. Run make install first.'; exit 1; fi"
    COMMAND ${CMAKE_COMMAND} -E echo "Done."
    VERBATIM
)
