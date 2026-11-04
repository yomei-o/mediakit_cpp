# OpenH264 2.6.0 (BSD 2-clause, see LICENSE), flattened, encoder only, C++ only (no assembly).
# Built only with -DMK_WITH_OPENH264=ON. The decoder is left out: FFmpeg's own H.264 decoder
# is already in the build, so only the encoder adds anything.
set(MK_OPENH264_DIR ${CMAKE_CURRENT_LIST_DIR})

add_library(mk_openh264 STATIC
    # codec/common/src
    ${MK_OPENH264_DIR}/common_tables.cpp      ${MK_OPENH264_DIR}/copy_mb.cpp
    ${MK_OPENH264_DIR}/cpu.cpp                ${MK_OPENH264_DIR}/crt_util_safe_x.cpp
    ${MK_OPENH264_DIR}/deblocking_common.cpp  ${MK_OPENH264_DIR}/expand_pic.cpp
    ${MK_OPENH264_DIR}/intra_pred_common.cpp  ${MK_OPENH264_DIR}/mc.cpp
    ${MK_OPENH264_DIR}/memory_align.cpp       ${MK_OPENH264_DIR}/sad_common.cpp
    ${MK_OPENH264_DIR}/utils.cpp              ${MK_OPENH264_DIR}/welsCodecTrace.cpp
    ${MK_OPENH264_DIR}/WelsTaskThread.cpp     ${MK_OPENH264_DIR}/WelsThread.cpp
    ${MK_OPENH264_DIR}/WelsThreadLib.cpp      ${MK_OPENH264_DIR}/WelsThreadPool.cpp
    # codec/encoder/core/src
    ${MK_OPENH264_DIR}/au_set.cpp             ${MK_OPENH264_DIR}/deblocking.cpp
    ${MK_OPENH264_DIR}/decode_mb_aux.cpp      ${MK_OPENH264_DIR}/encode_mb_aux.cpp
    ${MK_OPENH264_DIR}/encoder_data_tables.cpp ${MK_OPENH264_DIR}/encoder_ext.cpp
    ${MK_OPENH264_DIR}/encoder.cpp            ${MK_OPENH264_DIR}/get_intra_predictor.cpp
    ${MK_OPENH264_DIR}/md.cpp                 ${MK_OPENH264_DIR}/mv_pred.cpp
    ${MK_OPENH264_DIR}/nal_encap.cpp          ${MK_OPENH264_DIR}/paraset_strategy.cpp
    ${MK_OPENH264_DIR}/picture_handle.cpp     ${MK_OPENH264_DIR}/ratectl.cpp
    ${MK_OPENH264_DIR}/ref_list_mgr_svc.cpp   ${MK_OPENH264_DIR}/sample.cpp
    ${MK_OPENH264_DIR}/set_mb_syn_cabac.cpp   ${MK_OPENH264_DIR}/set_mb_syn_cavlc.cpp
    ${MK_OPENH264_DIR}/slice_multi_threading.cpp ${MK_OPENH264_DIR}/svc_base_layer_md.cpp
    ${MK_OPENH264_DIR}/svc_enc_slice_segment.cpp ${MK_OPENH264_DIR}/svc_encode_mb.cpp
    ${MK_OPENH264_DIR}/svc_encode_slice.cpp   ${MK_OPENH264_DIR}/svc_mode_decision.cpp
    ${MK_OPENH264_DIR}/svc_motion_estimate.cpp ${MK_OPENH264_DIR}/svc_set_mb_syn_cabac.cpp
    ${MK_OPENH264_DIR}/svc_set_mb_syn_cavlc.cpp ${MK_OPENH264_DIR}/wels_preprocess.cpp
    ${MK_OPENH264_DIR}/wels_task_base.cpp     ${MK_OPENH264_DIR}/wels_task_encoder.cpp
    ${MK_OPENH264_DIR}/wels_task_management.cpp
    # codec/encoder/plus/src
    ${MK_OPENH264_DIR}/welsEncoderExt.cpp
    # codec/processing/src
    ${MK_OPENH264_DIR}/AdaptiveQuantization.cpp ${MK_OPENH264_DIR}/BackgroundDetection.cpp
    ${MK_OPENH264_DIR}/ComplexityAnalysis.cpp ${MK_OPENH264_DIR}/denoise.cpp
    ${MK_OPENH264_DIR}/denoise_filter.cpp     ${MK_OPENH264_DIR}/downsample.cpp
    ${MK_OPENH264_DIR}/downsamplefuncs.cpp    ${MK_OPENH264_DIR}/imagerotate.cpp
    ${MK_OPENH264_DIR}/imagerotatefuncs.cpp   ${MK_OPENH264_DIR}/memory.cpp
    ${MK_OPENH264_DIR}/SceneChangeDetection.cpp ${MK_OPENH264_DIR}/ScrollDetection.cpp
    ${MK_OPENH264_DIR}/ScrollDetectionFuncs.cpp ${MK_OPENH264_DIR}/vaacalcfuncs.cpp
    ${MK_OPENH264_DIR}/vaacalculation.cpp     ${MK_OPENH264_DIR}/WelsFrameWork.cpp
    ${MK_OPENH264_DIR}/WelsFrameWorkEx.cpp
)
target_include_directories(mk_openh264 PUBLIC ${MK_OPENH264_DIR})
# version.h picks the real version string out of version_gen.h only with this defined.
target_compile_definitions(mk_openh264 PRIVATE GENERATED_VERSION_HEADER)
if(MSVC)
  target_compile_options(mk_openh264 PRIVATE /W0 /utf-8)
  target_compile_definitions(mk_openh264 PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(mk_openh264 PRIVATE -w)
endif()
if(IOS)
  # WelsThreadLib uses dispatch semaphores there: iOS has no unnamed POSIX semaphores.
  target_compile_definitions(mk_openh264 PRIVATE APPLE_IOS)
endif()
if(UNIX)
  find_package(Threads REQUIRED)
  target_link_libraries(mk_openh264 PUBLIC Threads::Threads)
endif()
