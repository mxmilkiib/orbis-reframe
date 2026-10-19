cmake_minimum_required(VERSION 3.24)
# NCNN_ROOT (below) is a search prefix for ncnn, as this policy makes it.
if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW)
endif()
project(orbis-reframe VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# moc / rcc run only on the targets that need them (AUTOMOC / AUTORCC set per
# target below) -- the Qt-free libraries and tools get no _autogen step.

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Gui)
# The bundled Lucide .svg icons are drawn through QImageReader by Qt's runtime
# SVG image-format plugin (Debian package qt6-svg-plugins -> libqsvg.so),
# loaded on demand -- no link-time Qt6::Svg dependency, so no qt6-svg-dev
# requirement. theme_test needs the plugin to draw them.
# Qt6::Multimedia (QAudioSink) drives audio playback --
# optional like FFmpeg/OpenCV below: orbis-reframe still builds without it,
# just silent, if the dev package isn't installed.
find_package(Qt6 QUIET COMPONENTS Multimedia)
if(NOT Qt6Multimedia_FOUND)
    message(STATUS "Qt6 Multimedia not found (sudo apt install qt6-multimedia-dev)"
        " -- orbis-reframe will build but stay silent (no audio playback)")
endif()
find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED) # project file (JSON) serialisation, src/core/Project
find_program(GLSLANG_VALIDATOR NAMES glslangValidator REQUIRED)

enable_testing()

set(SHADER_SOURCES
    src/render/shaders/equirect.vert
    src/render/shaders/equirect.frag
    src/render/shaders/fisheye_view.frag
    src/render/shaders/fisheye_stitch.comp
    src/render/shaders/fisheye_stitch_nv12.comp
    src/render/shaders/reframe_reproject.comp
    src/render/shaders/fisheye_view_nv12.comp
    src/render/shaders/inset.frag
    src/render/shaders/marker.frag
    src/render/shaders/region.frag
)
set(SHADER_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_BINARY_DIR})

# .glsl includes (GL_GOOGLE_include_directive) -- any change forces a reglsl.
file(GLOB SHADER_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/*.glsl)

# Each shader is compiled to SPIR-V as a C array header (glslangValidator --vn)
# and built into the binary through render_shaders (src/render/EmbeddedShaders),
# so nothing is read from disk at run time and the binary can live anywhere
# equirect.vert -> kSpv_equirect_vert.
set(SHADER_HEADERS "")
foreach(SHADER ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    string(REPLACE "." "_" SHADER_IDENT "kSpv_${SHADER_NAME}")
    set(SPIRV_HEADER ${SHADER_BINARY_DIR}/${SHADER_NAME}.h)
    add_custom_command(
        OUTPUT ${SPIRV_HEADER}
        COMMAND ${GLSLANG_VALIDATOR} -V --quiet
            -I${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders
            --vn ${SHADER_IDENT}
            ${CMAKE_CURRENT_SOURCE_DIR}/${SHADER} -o ${SPIRV_HEADER}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${SHADER} ${SHADER_INCLUDES}
        VERBATIM
    )
    list(APPEND SHADER_HEADERS ${SPIRV_HEADER})
endforeach()
add_custom_target(shaders DEPENDS ${SHADER_HEADERS})

# --- render_shaders: the SPIR-V above, compiled in. No Qt, no Vulkan link --
# just arrays -- so the Qt-free render_gpu_stitch can use it too.
add_library(render_shaders STATIC
    src/render/EmbeddedShaders.h
    src/render/EmbeddedShaders.cpp
    ${SHADER_HEADERS}
)
target_include_directories(render_shaders PUBLIC src/render PRIVATE ${SHADER_BINARY_DIR})

# --- FFmpeg: demux/decode/encode for the stitch layer, export and tools ---
find_package(PkgConfig)
if(PkgConfig_FOUND)
    # libavfilter: atempo time-stretch for clips at speed != 1 (ClipAudioReader).
    pkg_check_modules(FFMPEG IMPORTED_TARGET
        libavcodec libavformat libavutil libswscale libswresample libavfilter)
endif()
if(NOT FFMPEG_FOUND)
    message(STATUS "FFmpeg dev packages not found (sudo apt install libavcodec-dev"
        " libavformat-dev libavutil-dev libswscale-dev libswresample-dev)")
endif()

# --- OpenCV: optional. Drives residual 2-D stabilization (insv_optical +
# tools/residual_stab). Same "build it
# only if present" pattern as the FFmpeg check above. Deliberately only
# core/imgproc/video -- NOT cv::VideoCapture: opencv_videoio drags in GStreamer
# + gdk-pixbuf + a second libGL. (Frames come from stitch::NativeStitchSource or
# system FFmpeg instead.) ---
find_package(OpenCV QUIET COMPONENTS core imgproc video)
if(NOT OpenCV_FOUND)
    message(STATUS "OpenCV not found (sudo apt install libopencv-dev)"
        " -- skipping insv_optical and tools/residual_stab")
endif()
# OpenCV 5 moved what we use out of two modules: goodFeaturesToTrack from
# imgproc into `features`, findHomography / estimateAffinePartial2D from
# calib3d into `geometry`.
if(OpenCV_FOUND AND OpenCV_VERSION VERSION_GREATER_EQUAL 5)
    if(NOT TARGET opencv_features)
        message(STATUS "OpenCV ${OpenCV_VERSION} without its features module"
            " -- skipping everything that needs OpenCV")
        set(OpenCV_FOUND FALSE)
    endif()
    set(ORBIS_OPENCV_FEATURES opencv_features)
    set(ORBIS_OPENCV_GEOMETRY opencv_geometry)
else()
    set(ORBIS_OPENCV_FEATURES "")
    set(ORBIS_OPENCV_GEOMETRY opencv_calib3d)
endif()

# --- src/insv: .insv trailer parser (calibration + IMU/exposure telemetry), no
# FFmpeg/Qt dependency — see src/insv/InsvTrailer.h. Its own tiny lib
# (rather than folded into `insv`) so targets that only need trailer parsing
# don't pull the rest of the module.
add_library(insv_trailer STATIC
    src/insv/InsvTrailer.h
    src/insv/InsvTrailer.cpp
)
target_include_directories(insv_trailer PUBLIC src/insv)

add_executable(insv_trailer_dump tools/insv_trailer_dump/main.cpp)
target_link_libraries(insv_trailer_dump PRIVATE insv_trailer)

# --- src/insv: cleanroom .insv processing.
# GyroStabilizer + the stitch core: calibration-string parse
# and the CPU fisheye->equirect projection (productised from
# the since-removed tools/native_stitch_spike). No Qt / Vulkan / FFmpeg -- consumes
# only the raw calibration string via insv_trailer. `orbis_core` links this
# for GyroStabilizer, so it must stay dependency-free.
add_library(insv STATIC
    src/insv/BlurQuads.h
    src/insv/BlurQuads.cpp
    src/insv/GyroStabilizer.h
    src/insv/GyroStabilizer.cpp
    src/insv/Image.h
    src/insv/Calibration.h
    src/insv/Calibration.cpp
    src/insv/FisheyeProjection.h
    src/insv/FisheyeProjection.cpp
    src/insv/SeamAlignment.h
    src/insv/SeamAlignment.cpp
    src/insv/SeamDepth.h
    src/insv/SeamDepth.cpp
)
target_include_directories(insv PUBLIC src/insv)
target_link_libraries(insv PUBLIC insv_trailer Threads::Threads)

# --- orbis_version: the version string (src/core/Version) -- PROJECT_VERSION
# (set by `cz bump`, .cz.toml) plus `git describe` metadata, regenerated on
# every build by cmake/GitVersion.cmake.
set(ORBIS_VERSION_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/OrbisVersion.h)
add_custom_target(orbis_version_header
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DVERSION=${PROJECT_VERSION} -DOUT=${ORBIS_VERSION_HEADER}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GitVersion.cmake
    BYPRODUCTS ${ORBIS_VERSION_HEADER}
    VERBATIM)
add_library(orbis_version STATIC src/core/Version.h src/core/Version.cpp)
add_dependencies(orbis_version orbis_version_header)
target_include_directories(orbis_version PUBLIC src/core
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

# --- orbis_paths: where the data files (models) are at run time -- next to
# the executable (<exe>/../share/orbis-reframe), else, in a development build,
# the source tree (src/core/DataPaths). Packaged builds turn
# the fallback off so no build-machine path is left in the binary.
option(ORBIS_SOURCE_TREE_FALLBACK
    "Look for models in the source tree when they aren't installed (dev builds)" ON)
add_library(orbis_paths STATIC src/core/DataPaths.h src/core/DataPaths.cpp)
target_include_directories(orbis_paths PUBLIC src/core)
if(ORBIS_SOURCE_TREE_FALLBACK)
    target_compile_definitions(orbis_paths
        PRIVATE ORBIS_SOURCE_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources/models")
endif()

# --- color_lut: .cube 3D LUTs and color::Grade, what every layer that grades
# shares (the model, the GPU stitch, the CPU export). Dependency-free.
add_library(color_lut STATIC src/core/ColorLut.h src/core/ColorLut.cpp)
target_include_directories(color_lut PUBLIC src/core)

# --- orbis_core: the src/core edit model (ClipItem, Timeline, Project, ...).
# Qt Core + JSON only -- no FFmpeg / Vulkan -- so the model tests build
# without the FFmpeg block below. src/stitch and src/privacy are on the path
# for the header-only POD settings Project.h / ClipItem.h store
# (stitch::PreviewStitchConfig, privacy::BlurAddition / BlurRemoval).
add_library(orbis_core STATIC
    src/core/AudioLimits.h
    src/core/Autosave.cpp
    src/core/Autosave.h
    src/core/ClipItem.cpp
    src/core/ClipItem.h
    src/core/TrackClip.cpp
    src/core/TrackClip.h
    src/core/InsetGeometry.h
    src/core/KeyTrack.h
    src/core/Log.h
    src/core/MediaPool.cpp
    src/core/MediaPool.h
    src/core/Timeline.cpp
    src/core/Timeline.h
    src/core/Project.cpp
    src/core/Project.h
    src/core/ProjectFolder.cpp
    src/core/ProjectFolder.h
    src/core/ProjectLibrary.cpp
    src/core/ProjectLibrary.h
    src/core/OutputSettings.h
    src/core/ReframeTrack.cpp
    src/core/ReframeTrack.h
    src/core/Title.h
    src/core/Transition.h
)
set_target_properties(orbis_core PROPERTIES AUTOMOC ON)
target_include_directories(orbis_core PUBLIC src/core src/stitch src/privacy)
target_link_libraries(orbis_core PUBLIC Qt6::Core nlohmann_json::nlohmann_json insv color_lut)

# --- privacy_data: the privacy blur's plate-scan result + its cache file
# (src/privacy/PlateScanData). Dependency-free, so the app can always apply a
# scan it has; making one needs privacy_scan (OpenCV + ncnn, below).
add_library(privacy_data STATIC
    src/privacy/PlateScanData.h
    src/privacy/PlateScanData.cpp
    src/privacy/BlurAddition.h
    src/privacy/BlurRemoval.h
)
target_include_directories(privacy_data PUBLIC src/privacy)
target_link_libraries(privacy_data PUBLIC insv)

# --- render_gpu_stitch: headless Vulkan compute port of the src/insv/ CPU
# fisheye->equirect stitch. Its own
# VkInstance/VkDevice so it works from a CLI tool or inside the Qt app. Links
# Vulkan + insv (for insv::StitchGpuParams, the shared geometry struct) only --
# no Qt, no FFmpeg. Consumed by NativeStitchSource via stitch.
add_library(render_gpu_stitch STATIC
    src/render/LensFrame.h
    src/render/VulkanFisheyeStitcher.h
    src/render/VulkanFisheyeStitcher.cpp
    src/render/VulkanFisheyeStitcherImpl.h
    src/render/VulkanFisheyeStitcherSetup.cpp
    src/render/VulkanLut.h
    src/render/VulkanLut.cpp
)
target_include_directories(render_gpu_stitch PUBLIC src/render)
target_link_libraries(render_gpu_stitch PUBLIC insv color_lut Vulkan::Vulkan PRIVATE render_shaders)

# --- insv_demux: the one FFmpeg-dependent piece of src/insv/ (decode a fisheye
# frame from an .insv at time T). Separate lib so the projection/gyro core above
# stays dependency-free. Still cleanroom -- no Qt / Vulkan.
if(FFMPEG_FOUND)
    add_library(insv_demux STATIC
        src/insv/InsvDemux.h
        src/insv/InsvDemux.cpp
        # The demux / decode / seek core of InsvDemux and VulkanVideoDecoder.
        src/insv/LensDemuxer.h
        src/insv/LensDemuxer.cpp
        # Media information panel: container / stream / trailer facts.
        src/insv/InsvMediaInfo.h
        src/insv/InsvMediaInfo.cpp
        # GPU-resident decode via FFmpeg's Vulkan hwaccel.
        src/insv/VulkanVideoDecoder.h
        src/insv/VulkanVideoDecoder.cpp
        # Audio playback: its own file handle, independent
        # of the fisheye decode above -- no stitching involved.
        src/insv/InsvAudioDemux.h
        src/insv/InsvAudioDemux.cpp
        src/insv/ClipAudioReader.h
        src/insv/ClipAudioReader.cpp
        src/insv/AudioMixer.h
        src/insv/AudioMixer.cpp
        # Audio noise reduction, per mic clip: "MC: Wind reduction"
        # (WindReducer), "Wind reduction" (GeneralWindReducer) and
        # "Hiss / hum" (HissHumReducer); the voice modes (VoiceIsolator)
        # below, with ONNX Runtime.
        src/insv/NoiseReduction.h
        src/insv/NoiseFilter.h
        src/insv/NoiseFilter.cpp
        src/insv/Rdft.h
        src/insv/WindReducer.h
        src/insv/WindReducer.cpp
        src/insv/GeneralWindReducer.h
        src/insv/GeneralWindReducer.cpp
        src/insv/HissHumReducer.h
        src/insv/HissHumReducer.cpp
        src/insv/AudioPeaks.h
        src/insv/AudioPeaks.cpp
    )
    target_include_directories(insv_demux PUBLIC src/insv)
    target_link_libraries(insv_demux PUBLIC insv Vulkan::Vulkan PRIVATE PkgConfig::FFMPEG)

    # The "Voice only" / "Voice + ambience" noise modes: DeepFilterNet 3
    # (resources/models/deepfilternet3) run with ONNX Runtime -- optional
    # (libonnxruntime-dev); without it those modes are hidden. Only its
    # headers are needed to build: VoiceIsolator dlopen()s the library on
    # first use (so the app runs without it, and Debian's build's load-time
    # log spam can be silenced). ONNXRUNTIME_ROOT points at an unpacked copy
    # instead of the system one.
    find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
        HINTS ${ONNXRUNTIME_ROOT}/include PATH_SUFFIXES onnxruntime)
    find_library(ONNXRUNTIME_LIBRARY onnxruntime
        HINTS ${ONNXRUNTIME_ROOT}/lib ${ONNXRUNTIME_ROOT}/lib/x86_64-linux-gnu)
    if(ONNXRUNTIME_INCLUDE_DIR)
        message(STATUS "ONNX Runtime headers: ${ONNXRUNTIME_INCLUDE_DIR} -- voice noise modes on")
        target_sources(insv_demux PRIVATE src/insv/VoiceIsolator.h src/insv/VoiceIsolator.cpp)
        target_include_directories(insv_demux PRIVATE ${ONNXRUNTIME_INCLUDE_DIR})
        target_link_libraries(insv_demux PRIVATE ${CMAKE_DL_LIBS})
        target_compile_definitions(insv_demux PUBLIC INSV_HAVE_VOICE)
        # The headers' ORT_API_VERSION is the runtime's 1.x minor version,
        # after which Debian names it: libonnxruntime1.23, .so.1.23 (Debian
        # 13: 1.21). The .deb's Depends and the "install it" hint use it.
        file(STRINGS ${ONNXRUNTIME_INCLUDE_DIR}/onnxruntime_c_api.h ORT_API_LINE
            REGEX "^#define ORT_API_VERSION [0-9]+")
        string(REGEX MATCH "[0-9]+$" ORT_API_MINOR "${ORT_API_LINE}")
        set(ORBIS_ORT_PACKAGE libonnxruntime1.${ORT_API_MINOR})
        target_compile_definitions(insv_demux PUBLIC INSV_ORT_PACKAGE="${ORBIS_ORT_PACKAGE}")
        if(ONNXRUNTIME_LIBRARY)
            target_compile_definitions(insv_demux PRIVATE INSV_ORT_LIBRARY="${ONNXRUNTIME_LIBRARY}")
        endif()
    else()
        message(STATUS "ONNX Runtime not found (libonnxruntime-dev) -- voice noise modes off")
    endif()
endif()

# --- insv_optical: the OpenCV-dependent piece of src/insv/ -- residual 2-D
# stabilization (ResidualPanEstimator: feature tracking + optical flow -> a
# per-frame yaw/pitch correction). Separate lib so the core stays OpenCV-free;
# the apply side (ResidualTrack::sample) is header-only in ResidualStab.h.
if(OpenCV_FOUND)
    add_library(insv_optical STATIC
        src/insv/ResidualStab.h
        src/insv/ResidualPanEstimator.h
        src/insv/ResidualPanEstimator.cpp
        src/insv/SyncEstimator.h
        src/insv/SyncEstimator.cpp
    )
    target_include_directories(insv_optical PUBLIC src/insv PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(insv_optical PUBLIC insv
        PRIVATE opencv_core opencv_imgproc opencv_video ${ORBIS_OPENCV_FEATURES})
endif()

if(FFMPEG_FOUND)
    # --- tools/insv_stitch: smoke CLI. Turns an .insv into an
    # equirect through src/insv/ alone -- the standalone proof of the cleanroom
    # module. Links insv + insv_demux + insv_trailer, nothing else.
    add_executable(insv_stitch tools/insv_stitch/main.cpp)
    target_link_libraries(insv_stitch PRIVATE insv insv_demux insv_trailer)

    # --- tools/audio_filter_bench: test bench for the audio noise filters --
    # decodes one audio stream (InsvAudioDemux) to WAV, runs the app's filters
    # on it and scores the result; run_all.sh adds DeepFilterNet / afftdn.
    add_executable(audio_filter_bench tools/audio_filter_bench/main.cpp)
    target_link_libraries(audio_filter_bench PRIVATE insv_demux orbis_paths PkgConfig::FFMPEG)
endif()

# --- src/stitch: the Stitch Integration Layer. IStitchSource is served entirely by
# stitch::NativeStitchSource: trailer/calibration parse -> src/insv/
# Vulkan video decode + Vulkan compute fisheye->equirect stitch, in-process,
# frame-at-time-T.
if(FFMPEG_FOUND)
    # --- stitch: IStitchSource + NativeStitchSource + the clip probe. Pulls
    # in the src/insv/ cleanroom stitch core (insv + insv_demux, the latter
    # carrying the Vulkan-video decoder) and render_gpu_stitch (the Vulkan
    # compute stitch). Links no Qt.
    add_library(stitch STATIC
        src/stitch/IStitchSource.h
        src/stitch/NativeStitchSource.h
        src/stitch/NativeStitchSource.cpp
        src/stitch/ProbeClip.h
        src/stitch/ProbeClip.cpp
        src/stitch/ClipSeam.h
        src/stitch/ClipSeam.cpp
    )
    target_include_directories(stitch PUBLIC src/stitch)
    target_link_libraries(stitch PUBLIC insv insv_demux render_gpu_stitch)

    # --- src/export/reframe_flatten: the reproject + encode half of the flat
    # export -- take equirect frames (from stitch::NativeStitchSource in memory, or
    # an .mp4 intermediate), CPU-reproject each against the caller-baked pose
    # array, NVENC-encode + mux. Links system FFmpeg only.
    add_library(reframe_flatten STATIC
        src/export/ReframeFlatten.h
        src/export/ReframeFlatten.cpp
        src/export/ReframeRenderer.h
        src/export/ReframeRenderer.cpp
        # M7b: flat video insets -- decode, and composite into the output.
        src/export/FlatVideoReader.h
        src/export/FlatVideoReader.cpp
        src/export/InsetCompositor.h
        src/export/InsetCompositor.cpp
        src/export/PictureSource.h
    )
    # src/core for the Qt-free InsetGeometry.h (the shared inset placement).
    target_include_directories(reframe_flatten PUBLIC src/export src/core)
    target_link_libraries(reframe_flatten PUBLIC PkgConfig::FFMPEG Threads::Threads color_lut)

    # --- orbis_app: the whole app but main() -- the executable below, and
    # tests/main_window_test, which builds a MainWindow without a GPU.
    add_library(orbis_app STATIC
        src/ui/MainWindow.cpp
        src/ui/MainWindowColour.cpp
        src/ui/MainWindowTransitions.cpp
        src/ui/MainWindow.h
        src/ui/ClipBackend.cpp
        src/ui/ClipBackend.h
        src/ui/ClipMeasurements.cpp
        src/ui/ClipMeasurements.h
        src/ui/ShortcutHelp.cpp
        src/ui/ShortcutHelp.h
        src/ui/SmokeTest.cpp
        src/ui/SmokeTest.h
        src/ui/StabilizerCache.cpp
        src/ui/StabilizerCache.h
        src/ui/FrameViewCache.cpp
        src/ui/FrameViewCache.h
        src/ui/PlaybackController.cpp
        src/ui/PlaybackController.h
        src/ui/PlaybackControllerDecoder.cpp
        src/ui/FlatClipSource.cpp
        src/ui/PlaybackTuning.h
        src/ui/InsetFeed.cpp
        src/ui/InsetFeed.h
        src/ui/TitleRender.cpp
        src/ui/TitleRender.h
        src/ui/TitlesPanel.cpp
        src/ui/TitlesPanel.h
        src/ui/TransitionsPanel.cpp
        src/ui/TransitionsPanel.h
        src/ui/ClipViewSource.cpp
        src/ui/ClipViewSource.h
        src/ui/EditToolbar.cpp
        src/ui/EditToolbar.h
        src/ui/PlayerBar.cpp
        src/ui/PlayerBar.h
        src/ui/TimelineWidget.cpp
        src/ui/TimelineWidget.h
        src/ui/TimelineWidgetInput.cpp
        src/ui/TimelineWidgetLayout.h
        src/ui/TimelineWidgetPaint.cpp
        src/ui/TrackFiles.cpp
        src/ui/TrackFiles.h
        src/ui/WaveformCache.cpp
        src/ui/WaveformCache.h
        src/ui/ReframeExportController.cpp
        src/ui/ReframeExportController.h
        src/ui/ExportOptionsDialog.cpp
        src/ui/ExportOptionsDialog.h
        src/ui/ExportRunner.cpp
        src/ui/ExportRunner.h
        src/ui/RenderJobs.cpp
        src/ui/RenderJobs.h
        src/ui/SettingsSidebar.cpp
        src/ui/SettingsSidebar.h
        src/ui/TabSidebar.cpp
        src/ui/TabSidebar.h
        src/ui/MediaImporter.cpp
        src/ui/MediaImporter.h
        src/ui/MediaPoolPanel.cpp
        src/ui/MediaPoolPanel.h
        src/ui/MediaProbe.cpp
        src/ui/MediaProbe.h
        src/ui/ThumbnailCache.cpp
        src/ui/ThumbnailCache.h
        src/ui/SettingsPanel.cpp
        src/ui/SettingsPanel.h
        src/ui/panels/StitchingPanel.cpp
        src/ui/panels/StitchingPanel.h
        src/ui/panels/ReframePanel.cpp
        src/ui/panels/ReframePanel.h
        src/ui/panels/EaseCombo.h
        src/ui/panels/StabilizationPanel.cpp
        src/ui/panels/StabilizationPanel.h
        src/ui/panels/AudioPanel.cpp
        src/ui/panels/AudioPanel.h
        src/ui/panels/ColourPanel.cpp
        src/ui/panels/ColourPanel.h
        src/ui/panels/OverlayPanel.cpp
        src/ui/panels/OverlayPanel.h
        src/ui/panels/TitleTextPanel.cpp
        src/ui/panels/TitleTextPanel.h
        src/ui/panels/TransitionPanel.cpp
        src/ui/panels/TransitionPanel.h
        src/ui/panels/PrivacyPanel.cpp
        src/ui/panels/PrivacyPanel.h
        src/ui/PlateScanCache.cpp
        src/ui/PrivacyBlur.cpp
        src/ui/PrivacyBlur.h
        src/ui/PlateScanCache.h
        src/ui/panels/MediaInfoPanel.cpp
        src/ui/panels/MediaInfoPanel.h
        src/render/EquirectViewportWindow.cpp
        src/render/EquirectViewportRenderer.h
        src/render/EquirectViewportRenderer.cpp
        src/render/EquirectViewportRendererFrame.cpp
        src/render/EquirectViewportWindow.h
        src/render/FrameStats.h
        src/ui/AboutDialog.cpp
        src/ui/AboutDialog.h
        src/ui/UserManual.cpp
        src/ui/UserManual.h
        src/ui/ClipThumbnail.cpp
        src/ui/ClipThumbnail.h
        src/ui/NewProjectDialog.cpp
        src/ui/NewProjectDialog.h
        src/ui/OutputSettingsEditor.cpp
        src/ui/OutputSettingsEditor.h
        src/ui/ProjectFiles.cpp
        src/ui/ProjectFiles.h
        src/ui/ProjectSettingsDialog.cpp
        src/ui/ProjectSettingsDialog.h
        src/ui/ProjectLibraryPage.cpp
        src/ui/ProjectLibraryPage.h
        src/ui/SystemCheck.cpp
        src/ui/SystemCheck.h
        src/ui/Theme.cpp
        src/ui/Theme.h
    )
    set_target_properties(orbis_app PROPERTIES AUTOMOC ON)
    target_include_directories(orbis_app PUBLIC src/ui)
    # Audio playback needs Qt6::Multimedia, checked
    # optional above like FFmpeg/OpenCV -- orbis-reframe still builds without
    # it, just silently (ORBIS_REFRAME_HAVE_AUDIO unset), if the dev package
    # isn't installed.
    if(Qt6Multimedia_FOUND)
        target_sources(orbis_app PRIVATE src/ui/AudioPlayer.cpp src/ui/AudioPlayer.h)
        target_link_libraries(orbis_app PUBLIC Qt6::Multimedia)
        target_compile_definitions(orbis_app PUBLIC ORBIS_REFRAME_HAVE_AUDIO)
    endif()
    # reframe_flatten pulls system FFmpeg into orbis-reframe for the export's
    # decode/reproject/encode half.
    target_link_libraries(orbis_app PUBLIC
        Qt6::Widgets Qt6::Gui Vulkan::Vulkan orbis_core stitch reframe_flatten insv
        privacy_data render_shaders orbis_paths orbis_version)

    # The resources go into each executable: a static library's resource
    # initializer would be dropped by the linker.
    add_executable(orbis-reframe src/ui/main.cpp resources/resources.qrc)
    set_target_properties(orbis-reframe PROPERTIES AUTOMOC ON AUTORCC ON)
    target_link_libraries(orbis-reframe PRIVATE orbis_app)

    # Installed layout -- the same under /usr (.deb)
    # and in the tarball (cmake/Packaging.cmake): bin/orbis-reframe,
    # lib/orbis-reframe/ (bundled ncnn, found through $ORIGIN),
    # share/orbis-reframe/models/ (core::paths finds it from the binary's own
    # location), share/doc/orbis-reframe/ (licenses, the manual), the desktop
    # entry and icon.
    set_target_properties(orbis-reframe PROPERTIES
        INSTALL_RPATH "\$ORIGIN/../lib/orbis-reframe")
    install(TARGETS orbis-reframe RUNTIME DESTINATION bin)
    install(DIRECTORY resources/models/ DESTINATION share/orbis-reframe/models)
    install(FILES packaging/orbis-reframe.desktop DESTINATION share/applications)
    install(FILES packaging/orbis-reframe.svg DESTINATION share/icons/hicolor/scalable/apps)
    install(FILES LICENSE README.md THIRD_PARTY.md resources/icons/LICENSE.lucide
        DESTINATION share/doc/orbis-reframe)
    # Licenses of what is compiled in or bundled (THIRD_PARTY.md); the models'
    # own come with resources/models/. ncnn's is installed with ncnn, below.
    install(FILES packaging/licenses/nlohmann-json-LICENSE.MIT
        DESTINATION share/doc/orbis-reframe/licenses)
    install(FILES src/insv/LICENSE-MIT DESTINATION share/doc/orbis-reframe/licenses
        RENAME insv-LICENSE-MIT)
    install(FILES src/insv/LICENSE-APACHE DESTINATION share/doc/orbis-reframe/licenses
        RENAME insv-LICENSE-APACHE)
    install(DIRECTORY docs/manual/ DESTINATION share/doc/orbis-reframe/manual)

    add_executable(stitch_source_smoke tools/stitch_source_smoke/main.cpp)
    target_link_libraries(stitch_source_smoke PRIVATE stitch)

    # --- tools/playback_bench: per-mode decode / stitch / copy-back /
    # seek timings on the all-GPU path. Measure-only.
    add_executable(playback_bench tools/playback_bench/main.cpp)
    target_link_libraries(playback_bench PRIVATE stitch)

    # --- tools/view_compare: the fused fisheye -> view pass against the
    # equirect + reproject path (PSNR / sharpness per pose). Measure-only.
    add_executable(view_compare tools/view_compare/main.cpp)
    target_link_libraries(view_compare PRIVATE stitch)

    # --- tools/reframe_export: parse args, load the --keys file into
    # a real ReframeTrack, bake one pose per output frame, then drive
    # stitch::NativeStitchSource -> exportpipe::FlattenWriter (reproject + NVENC).
    add_executable(reframe_export tools/reframe_export/main.cpp)
    target_include_directories(reframe_export PRIVATE src/stitch src/export)
    target_link_libraries(reframe_export PRIVATE orbis_core stitch reframe_flatten)

    # --- tools/residual_stab: residual 2-D stabilization end to end.
    # stitch::NativeStitchSource (in-process GPU stitch) -> grayscale equirect -> insv::ResidualPanEstimator,
    # with the gyro de-rotation (insv::GyroStabilizer) folded in; optionally
    # renders a de-rotated / de-rotated+residual-2D clip through the GPU reproject
    # path (exportpipe::FlattenWriter). Retires the two-binary optflow_stab_spike.
    if(OpenCV_FOUND)
        # --- privacy_scan: the privacy blur's number-plate scan
        # (src/privacy/PlateScanner): gyro-levelled
        # tiles -> YOLOv9 plate model (resources/models/plates) on ncnn / Vulkan
        # -> tracks + quad tracker -> regions for the stitch to mosaic. ncnn
        # isn't packaged in Debian: an unpacked release
        # (https://github.com/Tencent/ncnn/releases, *-ubuntu-2404-shared) via
        # NCNN_ROOT=<dir with lib/cmake/ncnn>, or an installed one. Without it
        # the app can't scan (the Privacy panel says so) but still applies a
        # scan it has.
        # scripts/fetch_ncnn.sh puts the pinned release here; NCNN_ROOT overrides.
        set(NCNN_FETCHED ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ncnn/ncnn-20260526-ubuntu-2404-shared)
        if(NOT NCNN_ROOT AND EXISTS ${NCNN_FETCHED}/lib/cmake/ncnn)
            set(NCNN_ROOT ${NCNN_FETCHED})
        endif()
        if(NCNN_ROOT)
            find_package(ncnn CONFIG PATHS ${NCNN_ROOT}/lib/cmake/ncnn NO_DEFAULT_PATH)
        else()
            find_package(ncnn CONFIG QUIET)
        endif()
        if(ncnn_FOUND AND TARGET ${ORBIS_OPENCV_GEOMETRY} AND TARGET opencv_video)
            add_library(privacy_scan STATIC
                src/privacy/PlateScanner.h src/privacy/PlateScanner.cpp
                src/privacy/PlateTracking.h src/privacy/PlateTracking.cpp
                src/privacy/StitchFrameSource.h src/privacy/StitchFrameSource.cpp)
            target_include_directories(privacy_scan PUBLIC src/privacy PRIVATE ${OpenCV_INCLUDE_DIRS})
            target_link_libraries(privacy_scan PUBLIC privacy_data stitch
                PRIVATE orbis_paths ncnn opencv_core opencv_imgproc opencv_video
                ${ORBIS_OPENCV_FEATURES} ${ORBIS_OPENCV_GEOMETRY})


            # tools/plate_scan: the scan from the command line, plus stills of
            # the blur through the app's stitch paths (equirect and fused view).
            add_executable(plate_scan tools/plate_scan/main.cpp)
            target_include_directories(plate_scan PRIVATE src/insv src/stitch)
            target_link_libraries(plate_scan PRIVATE stitch privacy_scan)

            target_sources(orbis_app PRIVATE src/ui/PlateScanJob.cpp src/ui/PlateScanJob.h
                src/ui/PlateJobController.cpp src/ui/PlateJobController.h)
            # ncnn isn't in Debian: installs ship it next to the app, in
            # lib/orbis-reframe/ (orbis-reframe's INSTALL_RPATH, below).
            install(IMPORTED_RUNTIME_ARTIFACTS ncnn LIBRARY DESTINATION lib/orbis-reframe)
            install(FILES packaging/licenses/ncnn-LICENSE.txt
                DESTINATION share/doc/orbis-reframe/licenses)
            target_link_libraries(orbis_app PUBLIC privacy_scan)
            target_compile_definitions(orbis_app PUBLIC ORBIS_REFRAME_HAVE_PLATE_SCAN)
        else()
            message(STATUS "ncnn not found (scripts/fetch_ncnn.sh, or NCNN_ROOT=<unpacked ncnn release>)"
                " -- no plate scan")
        endif()

        # --- clip_sync: per-clip video<->IMU sync fine-tune --
        # NativeStitchSource windows -> insv::SyncEstimator. OpenCV
        # via insv_optical, so optional: without it the app skips the
        # fine-tune.
        add_library(clip_sync STATIC src/stitch/ClipSync.h src/stitch/ClipSync.cpp)
        target_include_directories(clip_sync PUBLIC src/stitch)
        target_link_libraries(clip_sync PUBLIC stitch insv_optical)

        add_executable(residual_stab tools/residual_stab/main.cpp)
        target_include_directories(residual_stab PRIVATE src/insv src/stitch src/export)
        target_link_libraries(residual_stab PRIVATE stitch insv_optical reframe_flatten clip_sync)

        # --- subject_track: object tracking (src/track/) -- a gyro-stabilized
        # rectilinear crop around the subject, OpenCV CSRT tracking it and
        # VitTrack (ONNX via opencv_dnn, resources/models/vittrack) scoring it,
        # the path reduced to reframe keys. Needs the contrib tracking + dnn
        # modules (libopencv-contrib-dev); without them the app hides "Track
        # subject" and tools/track_subject isn't built.
        if(TARGET opencv_tracking AND TARGET opencv_dnn AND TARGET opencv_imgcodecs)
            add_library(subject_track STATIC
                src/track/TrackGeometry.h
                src/track/SubjectTracker.h
                src/track/SubjectTracker.cpp
            )
            target_include_directories(subject_track PUBLIC src/track
                PRIVATE src/insv src/export ${OpenCV_INCLUDE_DIRS})
            target_link_libraries(subject_track PUBLIC insv reframe_flatten
                PRIVATE orbis_paths opencv_core opencv_imgproc opencv_video opencv_dnn opencv_tracking)

            # tools/track_subject: the SubjectTracker CLI (compares trackers,
            # renders a follow-cam).
            add_executable(track_subject tools/track_subject/main.cpp)
            target_include_directories(track_subject PRIVATE src/insv src/stitch src/export
                ${OpenCV_INCLUDE_DIRS})
            target_link_libraries(track_subject PRIVATE stitch subject_track reframe_flatten
                opencv_core opencv_imgproc opencv_imgcodecs)

            target_sources(orbis_app PRIVATE src/ui/SubjectTrackJob.cpp src/ui/SubjectTrackJob.h
                src/ui/SubjectTrackController.cpp src/ui/SubjectTrackController.h)
            target_link_libraries(orbis_app PUBLIC subject_track)
            target_compile_definitions(orbis_app PUBLIC ORBIS_REFRAME_HAVE_TRACKING)
        else()
            message(STATUS "OpenCV tracking/dnn/imgcodecs modules not found -- no subject tracking"
                " (libopencv-contrib-dev)")
        endif()

        # Per-clip gyro sync fine-tune in the app; without
        # OpenCV the app keeps the camera's timing.
        target_link_libraries(orbis_app PUBLIC clip_sync)
        target_compile_definitions(orbis_app PUBLIC ORBIS_REFRAME_HAVE_CLIP_SYNC)
    endif()

else()
    message(STATUS "Skipping orbis-reframe and stitch — need the FFmpeg dev packages")
endif()

# --- tests/support/test_media: WAVs and short videos the tests write
# themselves (FFmpeg's MPEG-4 encoder), so no test needs footage.
if(FFMPEG_FOUND)
    add_library(test_media STATIC tests/support/TestMedia.cpp tests/support/TestMedia.h)
    target_include_directories(test_media PUBLIC tests)
    target_link_libraries(test_media PRIVATE PkgConfig::FFMPEG)
endif()

# --- the src/core edit model's tests, one per area (tests/support/ModelTest.h
# is what they share). No FFmpeg / Vulkan dependency, so these build
# regardless of the block above. Run with `ctest` from the build dir (or
# the binaries directly).
foreach(model_test clip_item_test timeline_test timeline_tracks_test stabilization_test
        project_test color_lut_test)
    add_executable(${model_test} tests/${model_test}.cpp tests/support/ModelTest.h)
    target_include_directories(${model_test} PRIVATE tests)
    target_link_libraries(${model_test} PRIVATE orbis_core)
    add_test(NAME ${model_test} COMMAND ${model_test})
endforeach()

# Help > User manual's link per version (release tag or main).
add_executable(user_manual_test tests/user_manual_test.cpp
    src/ui/UserManual.cpp src/ui/UserManual.h)
target_include_directories(user_manual_test PRIVATE src/ui)
target_link_libraries(user_manual_test PRIVATE Qt6::Core)
add_test(NAME user_manual_test COMMAND user_manual_test)

# TimelineWidget mouse handling on the offscreen platform.
find_package(Qt6 QUIET COMPONENTS Test)
if(Qt6Test_FOUND)
    add_executable(timeline_widget_test tests/timeline_widget_test.cpp
        src/ui/TimelineWidget.cpp src/ui/TimelineWidget.h
        src/ui/TimelineWidgetInput.cpp src/ui/TimelineWidgetLayout.h src/ui/TimelineWidgetPaint.cpp
        src/ui/Theme.cpp src/ui/Theme.h)
    set_target_properties(timeline_widget_test PROPERTIES AUTOMOC ON)
    target_include_directories(timeline_widget_test PRIVATE src/ui)
    target_link_libraries(timeline_widget_test PRIVATE orbis_core Qt6::Widgets Qt6::Test)
    add_test(NAME timeline_widget_test COMMAND timeline_widget_test)

    # The project library's start screen and New project dialog, found by
    # objectName (no GPU).
    add_executable(project_library_test tests/project_library_test.cpp
        src/ui/ProjectLibraryPage.cpp src/ui/ProjectLibraryPage.h
        src/ui/NewProjectDialog.cpp src/ui/NewProjectDialog.h
        src/ui/OutputSettingsEditor.cpp src/ui/OutputSettingsEditor.h
        src/ui/ClipThumbnail.cpp src/ui/ClipThumbnail.h
        src/ui/Theme.cpp src/ui/Theme.h
        resources/resources.qrc)
    set_target_properties(project_library_test PROPERTIES AUTOMOC ON AUTORCC ON)
    target_include_directories(project_library_test PRIVATE src/ui)
    target_link_libraries(project_library_test PRIVATE orbis_core Qt6::Widgets Qt6::Test)
    add_test(NAME project_library_test COMMAND project_library_test)

    # The whole main window offscreen, without a GPU: no Vulkan instance and
    # stand-in clips (ClipBackend). Needs the app (FFmpeg).
    if(TARGET orbis_app)
        add_executable(main_window_test tests/main_window_test.cpp resources/resources.qrc)
        set_target_properties(main_window_test PROPERTIES AUTOMOC ON AUTORCC ON)
        target_link_libraries(main_window_test PRIVATE orbis_app test_media Qt6::Test)
        add_test(NAME main_window_test COMMAND main_window_test)

        # The privacy blur's data side against a synthetic plate scan.
        add_executable(privacy_blur_test tests/privacy_blur_test.cpp)
        target_link_libraries(privacy_blur_test PRIVATE orbis_app)
        add_test(NAME privacy_blur_test COMMAND privacy_blur_test)

        # The edit model as the renderers are handed it (mix, overlays, export
        # jobs, suggested file names).
        add_executable(render_jobs_test tests/render_jobs_test.cpp)
        target_link_libraries(render_jobs_test PRIVATE orbis_app)
        add_test(NAME render_jobs_test COMMAND render_jobs_test)
    endif()

    # The left sidebar and its Media tab (the media pool), found by
    # objectName (no GPU, no FFmpeg: no thumbnails).
    add_executable(media_pool_test tests/media_pool_test.cpp
        src/ui/MediaPoolPanel.cpp src/ui/MediaPoolPanel.h
        src/ui/ThumbnailCache.cpp src/ui/ThumbnailCache.h
        src/ui/TabSidebar.cpp src/ui/TabSidebar.h
        src/ui/Theme.cpp src/ui/Theme.h
        resources/resources.qrc)
    set_target_properties(media_pool_test PROPERTIES AUTOMOC ON AUTORCC ON)
    target_include_directories(media_pool_test PRIVATE src/ui)
    target_link_libraries(media_pool_test PRIVATE orbis_core Qt6::Widgets Qt6::Test)
    add_test(NAME media_pool_test COMMAND media_pool_test)

    # The edit toolbar above the timeline, found by objectName (no GPU).
    add_executable(edit_toolbar_test tests/edit_toolbar_test.cpp
        src/ui/EditToolbar.cpp src/ui/EditToolbar.h
        src/ui/Theme.cpp src/ui/Theme.h
        resources/resources.qrc)
    set_target_properties(edit_toolbar_test PROPERTIES AUTOMOC ON AUTORCC ON)
    target_include_directories(edit_toolbar_test PRIVATE src/ui)
    target_link_libraries(edit_toolbar_test PRIVATE Qt6::Widgets Qt6::Test)
    add_test(NAME edit_toolbar_test COMMAND edit_toolbar_test)

    # The player bar under the viewport, found by objectName (no GPU).
    add_executable(player_bar_test tests/player_bar_test.cpp
        src/ui/PlayerBar.cpp src/ui/PlayerBar.h
        src/ui/Theme.cpp src/ui/Theme.h
        resources/resources.qrc)
    set_target_properties(player_bar_test PROPERTIES AUTOMOC ON AUTORCC ON)
    target_include_directories(player_bar_test PRIVATE src/ui)
    target_link_libraries(player_bar_test PRIVATE Qt6::Widgets Qt6::Test)
    add_test(NAME player_bar_test COMMAND player_bar_test)

    # The theme's tokens, stylesheet template and icon tinting.
    add_executable(theme_test tests/theme_test.cpp src/ui/Theme.cpp src/ui/Theme.h
        resources/resources.qrc)
    set_target_properties(theme_test PROPERTIES AUTOMOC ON AUTORCC ON)
    target_include_directories(theme_test PRIVATE src/ui)
    target_link_libraries(theme_test PRIVATE Qt6::Widgets)
    add_test(NAME theme_test COMMAND theme_test)
endif()

# insv::AudioMixer on generated WAVs -- needs FFmpeg, like insv_demux.
if(TARGET insv_demux)
    add_executable(audio_mix_test tests/audio_mix_test.cpp)
    target_link_libraries(audio_mix_test PRIVATE insv_demux orbis_paths test_media)
    add_test(NAME audio_mix_test COMMAND audio_mix_test)
    # The voice-mode checks need the model even when the source-tree fallback
    # is off (package builds, scripts/package.sh).
    set_tests_properties(audio_mix_test PROPERTIES ENVIRONMENT
        "ORBIS_DFN_MODEL_DIR=${CMAKE_CURRENT_SOURCE_DIR}/resources/models/deepfilternet3")
endif()

# The right sidebar (settings tabs and sections) and the Project settings
# dialog, found by objectName. Its Audio and Media information sections use
# insv_demux (noise modes, the media probe's types), hence FFmpeg.
if(TARGET insv_demux AND Qt6Test_FOUND)
    add_executable(settings_sidebar_test tests/settings_sidebar_test.cpp
        src/ui/SettingsSidebar.cpp src/ui/SettingsSidebar.h
        src/ui/SettingsPanel.cpp src/ui/SettingsPanel.h
        src/ui/TabSidebar.cpp src/ui/TabSidebar.h
        src/ui/panels/StitchingPanel.cpp src/ui/panels/StitchingPanel.h
        src/ui/panels/ReframePanel.cpp src/ui/panels/ReframePanel.h
        src/ui/panels/StabilizationPanel.cpp src/ui/panels/StabilizationPanel.h
        src/ui/panels/AudioPanel.cpp src/ui/panels/AudioPanel.h
        src/ui/panels/ColourPanel.cpp src/ui/panels/ColourPanel.h
        src/ui/panels/OverlayPanel.cpp src/ui/panels/OverlayPanel.h
        src/ui/panels/TitleTextPanel.cpp src/ui/panels/TitleTextPanel.h
        src/ui/panels/TransitionPanel.cpp src/ui/panels/TransitionPanel.h
        src/ui/panels/PrivacyPanel.cpp src/ui/panels/PrivacyPanel.h
        src/ui/panels/MediaInfoPanel.cpp src/ui/panels/MediaInfoPanel.h
        src/ui/ProjectSettingsDialog.cpp src/ui/ProjectSettingsDialog.h
        src/ui/OutputSettingsEditor.cpp src/ui/OutputSettingsEditor.h
        src/ui/Theme.cpp src/ui/Theme.h
        resources/resources.qrc)
    set_target_properties(settings_sidebar_test PROPERTIES AUTOMOC ON AUTORCC ON)
    target_include_directories(settings_sidebar_test PRIVATE src/ui)
    target_link_libraries(settings_sidebar_test PRIVATE orbis_core insv_demux Qt6::Widgets
        Qt6::Test)
    add_test(NAME settings_sidebar_test COMMAND settings_sidebar_test)
endif()

# M7b: the flat-video reader and the inset compositor on generated video.
if(TARGET reframe_flatten)
    add_executable(inset_test tests/inset_test.cpp)
    target_include_directories(inset_test PRIVATE src/core)
    target_link_libraries(inset_test PRIVATE reframe_flatten test_media)
    add_test(NAME inset_test COMMAND inset_test)
endif()

# --- tests/insv_test: the cleanroom stitch core (calibration parse, radial
# model, linear algebra, FisheyeStitcher geometry, the trailer thumbnail). All
# synthetic -- links only the dependency-free `insv` and `insv_trailer` libs, no
# Qt / FFmpeg / .insv file needed.
add_executable(insv_test tests/insv_test.cpp)
target_link_libraries(insv_test PRIVATE insv insv_trailer)
add_test(NAME insv_test COMMAND insv_test)

# The privacy blur's plate-scan data (lookup, spans, merge, cache file).
add_executable(privacy_test tests/privacy_test.cpp)
target_link_libraries(privacy_test PRIVATE privacy_data)
add_test(NAME privacy_test COMMAND privacy_test)

# Release packages are built with every feature (scripts/package.sh):
# fail here instead of quietly packaging an app without one.
option(ORBIS_REQUIRE_ALL_FEATURES "Fail if an optional feature's dependency is missing" OFF)
if(ORBIS_REQUIRE_ALL_FEATURES)
    set(missing "")
    if(NOT TARGET orbis-reframe)
        list(APPEND missing "the app itself (FFmpeg dev packages)")
    endif()
    if(NOT Qt6Multimedia_FOUND)
        list(APPEND missing "audio playback (qt6-multimedia-dev)")
    endif()
    if(NOT ONNXRUNTIME_INCLUDE_DIR)
        list(APPEND missing "voice modes (libonnxruntime-dev)")
    endif()
    if(NOT TARGET clip_sync)
        list(APPEND missing "gyro sync fine-tune (libopencv-dev)")
    endif()
    if(NOT TARGET subject_track)
        list(APPEND missing "subject tracking (libopencv-contrib-dev)")
    endif()
    if(NOT TARGET privacy_scan)
        list(APPEND missing "plate scan (scripts/fetch_ncnn.sh)")
    endif()
    if(missing)
        list(JOIN missing ", " missing)
        message(FATAL_ERROR "ORBIS_REQUIRE_ALL_FEATURES: missing ${missing}")
    endif()
endif()

include(cmake/Packaging.cmake)
