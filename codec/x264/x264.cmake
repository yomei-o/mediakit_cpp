# x264 (GPL 2+), flattened, 8-bit, C only (no assembly). Built only with -DMK_WITH_X264=ON.
set(MK_X264_DIR ${CMAKE_CURRENT_LIST_DIR})
add_library(mk_x264 STATIC
    ${MK_X264_DIR}/common_base.c        ${MK_X264_DIR}/common_bitstream.c   ${MK_X264_DIR}/common_cabac.c
    ${MK_X264_DIR}/common_common.c      ${MK_X264_DIR}/common_cpu.c         ${MK_X264_DIR}/common_dct.c
    ${MK_X264_DIR}/common_deblock.c     ${MK_X264_DIR}/common_frame.c       ${MK_X264_DIR}/common_macroblock.c
    ${MK_X264_DIR}/common_mc.c          ${MK_X264_DIR}/common_mvpred.c      ${MK_X264_DIR}/common_osdep.c
    ${MK_X264_DIR}/common_pixel.c       ${MK_X264_DIR}/common_predict.c     ${MK_X264_DIR}/common_quant.c
    ${MK_X264_DIR}/common_rectangle.c   ${MK_X264_DIR}/common_set.c         ${MK_X264_DIR}/common_tables.c
    ${MK_X264_DIR}/common_threadpool.c  ${MK_X264_DIR}/common_vlc.c
    ${MK_X264_DIR}/encoder_analyse.c    ${MK_X264_DIR}/encoder_api.c        ${MK_X264_DIR}/encoder_cabac.c
    ${MK_X264_DIR}/encoder_cavlc.c      ${MK_X264_DIR}/encoder_encoder.c    ${MK_X264_DIR}/encoder_lookahead.c
    ${MK_X264_DIR}/encoder_macroblock.c ${MK_X264_DIR}/encoder_me.c         ${MK_X264_DIR}/encoder_ratecontrol.c
    ${MK_X264_DIR}/encoder_set.c        ${MK_X264_DIR}/encoder_slicetype-cl.c
)
if(WIN32)
  target_sources(mk_x264 PRIVATE ${MK_X264_DIR}/common_win32thread.c)
endif()
target_include_directories(mk_x264 PUBLIC ${MK_X264_DIR})
if(MSVC)
  target_compile_options(mk_x264 PRIVATE /W0 /utf-8)
  target_compile_definitions(mk_x264 PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(UNIX)
  find_package(Threads REQUIRED)
  target_link_libraries(mk_x264 PUBLIC Threads::Threads m)
endif()
