# ThorVG 1.1.2, CPU engine with memory-only Lottie and embedded PNG/JPEG.
set(THORVG_VERSION 1.1.2)
set(THORVG_NEON_VECTOR_SUPPORT OFF)
if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID MATCHES "^ARM64")
    set(THORVG_NEON_VECTOR_SUPPORT ON)
endif()
configure_file(config.h.in ${CMAKE_CURRENT_BINARY_DIR}/config/config.h)
set(S ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(THORVG_SOURCES
    ${S}/common/tvgCompressor.cpp
    ${S}/common/tvgMath.cpp
    ${S}/common/tvgStr.cpp

    ${S}/renderer/tvgAccessor.cpp
    ${S}/renderer/tvgAnimation.cpp
    ${S}/renderer/tvgCanvas.cpp
    ${S}/renderer/tvgFill.cpp
    ${S}/renderer/tvgInitializer.cpp
    ${S}/renderer/tvgLoaderMgr.cpp
    ${S}/renderer/tvgPaint.cpp
    ${S}/renderer/tvgPicture.cpp
    ${S}/renderer/tvgRender.cpp
    ${S}/renderer/tvgSaver.cpp
    ${S}/renderer/tvgScene.cpp
    ${S}/renderer/tvgShape.cpp
    ${S}/renderer/tvgTaskScheduler.cpp
    ${S}/renderer/tvgText.cpp

    ${S}/renderer/cpu_engine/tvgSwBlendOp.cpp
    ${S}/renderer/cpu_engine/tvgSwFill.cpp
    ${S}/renderer/cpu_engine/tvgSwImage.cpp
    ${S}/renderer/cpu_engine/tvgSwMemPool.cpp
    ${S}/renderer/cpu_engine/tvgSwPostEffect.cpp
    ${S}/renderer/cpu_engine/tvgSwRaster.cpp
    ${S}/renderer/cpu_engine/tvgSwRenderer.cpp
    ${S}/renderer/cpu_engine/tvgSwRle.cpp
    ${S}/renderer/cpu_engine/tvgSwShape.cpp
    ${S}/renderer/cpu_engine/tvgSwStroke.cpp
    ${S}/renderer/cpu_engine/tvgSwUtil.cpp

    ${S}/loaders/raw/tvgRawLoader.cpp

    ${S}/loaders/lottie/tvgLottieAnimation.cpp
    ${S}/loaders/lottie/tvgLottieBuilder.cpp
    ${S}/loaders/lottie/tvgLottieExpressions.cpp
    ${S}/loaders/lottie/tvgLottieInterpolator.cpp
    ${S}/loaders/lottie/tvgLottieLoader.cpp
    ${S}/loaders/lottie/tvgLottieModel.cpp
    ${S}/loaders/lottie/tvgLottieModifier.cpp
    ${S}/loaders/lottie/tvgLottieParserHandler.cpp
    ${S}/loaders/lottie/tvgLottieParser.cpp
    ${S}/loaders/lottie/tvgLottieProperty.cpp

    ${S}/loaders/png/tvgLodePng.cpp
    ${S}/loaders/png/tvgLodePngUtil.cpp
    ${S}/loaders/png/tvgPngLoader.cpp

    ${S}/loaders/jpg/tvgJpgd.cpp
    ${S}/loaders/jpg/tvgJpgLoader.cpp
)
add_library(thorvg STATIC ${THORVG_SOURCES})
target_include_directories(thorvg PUBLIC inc src/loaders/lottie
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/config ${S}/common ${S}/renderer
    ${S}/renderer/cpu_engine ${S}/loaders ${S}/loaders/raw ${S}/loaders/png
    ${S}/loaders/jpg ${S}/savers)
target_compile_definitions(thorvg PUBLIC TVG_STATIC)
target_compile_options(thorvg PRIVATE /wd4100 /wd4244 /wd4267 /wd4456 /wd4457 /wd4458
    /wd4701 /wd4702 /wd4703 /wd4334 /wd4996)
