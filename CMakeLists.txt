cmake_minimum_required(VERSION 2.8)
project(upmux)

set(CMAKE_C_FLAGS_DEBUG "-gdwarf-2 -g3 -std=gnu99  -DHAVE_X264_MPEG2 -DHAVE_MMX -ffast-math -m64  -I. -mpreferred-stack-boundary=5  -fomit-frame-pointer -fno-tree-vectorize ")

set(CMAKE_C_FLAGS_RELEASE "-gdwarf-2 -std=gnu99 -O2 -march=native  -DHAVE_X264_MPEG2 -DHAVE_MMX -ffast-math -m64  -I. -mpreferred-stack-boundary=5  -fomit-frame-pointer -fno-tree-vectorize")

#-flto

set(CMAKE_BUILD_TYPE Debug)

include_directories( .  /usr/local/include )
link_directories( /usr/local/lib)


add_executable( upmux
                main.c
)

TARGET_LINK_LIBRARIES(upmux
    
    upipe
    upipe_modules
    upipe_pthread
    upump_ev
    upipe_av
    upipe_framers
    upipe_ts
    upipe_sws upipe_swr
    upipe_filters
    upipe_x264
    rt
    swscale
    avutil
    ev
    avcodec
    avformat
    avcodec
    xcb-shm
    xcb-xfixes
    xcb-render
    xcb-shape
    xcb
    X11
    asound
    SDL
    z
    swresample
    m
    pthread
    dl
)