cmake_minimum_required(VERSION 3.10)
project(ter-music LANGUAGES C)

# Linyaps（如意玲珑）应用 ID：与 scripts/build/build-linyaps.sh 的 APP_ID 保持一致
set(TER_MUSIC_APP_ID "org.yxzl.ter-music")

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 静态链接选项（仅将 FFmpeg 库静态链接，其他库动态链接）
# FFmpeg 的 soname 在不同 EL 版本间不同，是跨版本兼容的主要障碍。
# 其他库（ncurses、libcurl、libpng、libjpeg、pulse）的 soname
# 在 EL8/9/10 间稳定，使用动态链接即可。
option(STATIC_LINKING "Statically link FFmpeg only, other libs dynamic" OFF)

# Linyaps（如意玲珑）专用集成文件：systemd 用户单元（常驻后台播放）与
# D-Bus 激活服务（按需拉起后台播放）。这些文件只在 Linyaps 包中安装，
# 因为它们的 Exec/ExecStart 会被 ll-builder 重写为 `ll-cli run <appid> -- ...`，
# 其中包含仅容器内存在的路径；其他打包格式保持原样（默认 OFF）。
option(INSTALL_LINYAPS_INTEGRATION
       "Install Linyaps integration files (systemd user unit, D-Bus activation)" OFF)

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
pkg_check_modules(PIPEWIRE libpipewire-0.3)
pkg_check_modules(DBUS dbus-1)
pkg_check_modules(LIBCURL libcurl)
pkg_check_modules(LIBXML2 REQUIRED libxml-2.0)
# zlib — required for tar.gz language pack extraction
pkg_check_modules(ZLIB REQUIRED zlib)
message(STATUS "Using zlib: ${ZLIB_VERSION}")
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
if(PIPEWIRE_FOUND)
    set(AUDIO_BACKEND_INCLUDE_DIRS ${AUDIO_BACKEND_INCLUDE_DIRS} ${PIPEWIRE_INCLUDE_DIRS})
    set(AUDIO_BACKEND_CFLAGS ${AUDIO_BACKEND_CFLAGS} ${PIPEWIRE_CFLAGS_OTHER})
endif()

if(PULSE_FOUND AND ALSA_FOUND AND PIPEWIRE_FOUND)
    message(STATUS "Audio backend(s): PulseAudio + ALSA + PipeWire (runtime dlopen)")
elseif(PULSE_FOUND AND ALSA_FOUND)
    message(STATUS "Audio backend(s): PulseAudio + ALSA (runtime dlopen)")
elseif(PULSE_FOUND AND PIPEWIRE_FOUND)
    message(STATUS "Audio backend(s): PulseAudio + PipeWire (runtime dlopen)")
elseif(ALSA_FOUND AND PIPEWIRE_FOUND)
    message(STATUS "Audio backend(s): ALSA + PipeWire (runtime dlopen)")
elseif(PULSE_FOUND)
    message(STATUS "Audio backend(s): PulseAudio (runtime dlopen)")
elseif(ALSA_FOUND)
    message(STATUS "Audio backend(s): ALSA (runtime dlopen)")
elseif(PIPEWIRE_FOUND)
    message(STATUS "Audio backend(s): PipeWire (runtime dlopen)")
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
    src/org.yxzl.ter-music/main/main.c
    src/org.yxzl.ter-music/core/core.c
    src/org.yxzl.ter-music/player/player_local.c
    src/org.yxzl.ter-music/audio/audio.c
    src/org.yxzl.ter-music/audio/atempo.c
    src/org.yxzl.ter-music/audio/equalizer.c
    src/org.yxzl.ter-music/audio/audio_visualizer.c
    src/org.yxzl.ter-music/audio/segment_buffer.c
    src/org.yxzl.ter-music/audio/playback_thread.c
    src/org.yxzl.ter-music/audio/play_queue.c
    src/org.yxzl.ter-music/audio/backend_ops.c
    src/org.yxzl.ter-music/audio/backend/pulse.c
    src/org.yxzl.ter-music/audio/backend/alsa.c
    src/org.yxzl.ter-music/audio/backend/pipewire.c
    src/org.yxzl.ter-music/audio/progress/progress.c
    src/org.yxzl.ter-music/playlist/playlist.c
    src/org.yxzl.ter-music/playlist/cue_parser.c
    src/org.yxzl.ter-music/playlist/encoding.c
    src/org.yxzl.ter-music/playlist/ape_tag.c
    src/org.yxzl.ter-music/search/search.c
    src/org.yxzl.ter-music/library/library.c
    src/org.yxzl.ter-music/library/library_query.c
    src/org.yxzl.ter-music/library/browser/browser.c
    src/org.yxzl.ter-music/ui/ui.c
    src/org.yxzl.ter-music/ui/utf8.c
    src/org.yxzl.ter-music/ui/dialog.c
    src/org.yxzl.ter-music/ui/layout.c
    src/org.yxzl.ter-music/ui/visualizer.c
    src/org.yxzl.ter-music/ui/controls.c
    src/org.yxzl.ter-music/ui/progress_ui.c
    src/org.yxzl.ter-music/ui/playlist_render.c
    src/org.yxzl.ter-music/ui/mouse.c
    src/org.yxzl.ter-music/ui/util.c
    src/org.yxzl.ter-music/ui/menus.c
    src/org.yxzl.ter-music/ui/settings.c
    src/org.yxzl.ter-music/ui/history.c
    src/org.yxzl.ter-music/ui/favorites.c
    src/org.yxzl.ter-music/ui/playlist_view.c
    src/org.yxzl.ter-music/ui/info_view.c
    src/org.yxzl.ter-music/ui/help_view.c
    src/org.yxzl.ter-music/ui/language_view.c
    src/org.yxzl.ter-music/i18n/i18n.c
    src/org.yxzl.ter-music/ui/lyrics.c
    src/org.yxzl.ter-music/ui/braille/braille_art.c
    src/org.yxzl.ter-music/ui/braille/image_loader.c
    src/org.yxzl.ter-music/ui/scrollbar.c
    src/org.yxzl.ter-music/media/session.c
    src/org.yxzl.ter-music/media/rpc_common.c
    src/org.yxzl.ter-music/media/rpc_info.c
    src/org.yxzl.ter-music/media/rpc_control.c
    src/org.yxzl.ter-music/media/rpc_lyrics.c
    src/org.yxzl.ter-music/media/rpc_playlist.c
    src/org.yxzl.ter-music/media/rpc_queue.c
    src/org.yxzl.ter-music/media/rpc_job.c
    src/org.yxzl.ter-music/media/rpc_library.c
    src/org.yxzl.ter-music/media/rpc_config.c
    src/org.yxzl.ter-music/media/rpc_remote.c
    src/org.yxzl.ter-music/info/info.c
    src/org.yxzl.ter-music/cli/cli.c
    src/org.yxzl.ter-music/cli/cli_client.c
    src/org.yxzl.ter-music/cli/daemon.c
    src/org.yxzl.ter-music/app/open.c
    src/org.yxzl.ter-music/util/json.c
    src/org.yxzl.ter-music/remote/remote.c
    src/org.yxzl.ter-music/config/config.c
    src/org.yxzl.ter-music/config/migration.c
    src/org.yxzl.ter-music/config/crypto.c
    src/org.yxzl.ter-music/config/config_json.c
    src/org.yxzl.ter-music/logger/logger.c
)

# 包含头文件目录
target_include_directories(${PROJECT_NAME} PRIVATE
    include/org.yxzl.ter-music/
    include/org.yxzl.ter-music/i18n/
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
    ${ZLIB_INCLUDE_DIRS}
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
    ${ZLIB_LIBRARIES}
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
    TER_MUSIC_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
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
install(FILES data/help/help-quickstart-zh_CN.txt data/help/help-quickstart-en_US.txt
        DESTINATION share/ter-music/help
)

# Linyaps 集成（可选）：systemd 用户单元与 D-Bus 激活服务
if(INSTALL_LINYAPS_INTEGRATION)
    # $PREFIX/lib/systemd/user → 构建时被拷贝到 share/systemd/user 并导出到
    # 宿主 $XDG_DATA_DIRS/systemd/user，ExecStart 被重写为 ll-cli run 形式
    install(FILES data/linyaps/org.yxzl.ter-music.service
            DESTINATION lib/systemd/user
    )
    # $PREFIX/share/dbus-1/services → 被 ll-builder 导出到宿主的
    # /var/lib/linglong/entries/share/dbus-1/services（$XDG_DATA_DIRS/dbus-1/services），
    # 使 org.mpris.MediaPlayer2.ter_music 可按需 D-Bus 激活。
    # 注意：必须是 share/dbus-1/services，Linyaps 只导出该目录下的会话服务文件
    #（share/services 不会被导出，实测安装后宿主无激活文件）。
    install(FILES data/linyaps/org.mpris.MediaPlayer2.ter_music.service
            DESTINATION share/dbus-1/services
    )
    message(STATUS "Linyaps integration: systemd user unit + D-Bus activation enabled")
endif()

# Install language translation files
install(FILES data/lang/zh_CN.xml data/lang/en_US.xml
        DESTINATION share/ter-music/lang
)

# Uninstall target — removes files listed in install_manifest.txt
add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -E echo "Uninstalling ${PROJECT_NAME}..."
    COMMAND sh -c "if [ -f '${CMAKE_BINARY_DIR}/install_manifest.txt' ]; then xargs rm -f < '${CMAKE_BINARY_DIR}/install_manifest.txt'; else echo 'No install manifest. Run make install first.'; exit 1; fi"
    COMMAND ${CMAKE_COMMAND} -E echo "Done."
    VERBATIM
)
