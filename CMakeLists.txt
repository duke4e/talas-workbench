cmake_minimum_required(VERSION 3.28)

# Apple's linker resolves static archives in any order and warns about the
# repeats CMake otherwise adds; let CMake drop them where the linker allows.
# The defaults reach JUCE, whose functions create the Workbench app target.
foreach(policy CMP0156 CMP0179)
    if(POLICY ${policy})
        cmake_policy(SET ${policy} NEW)
        set(CMAKE_POLICY_DEFAULT_${policy} NEW)
    endif()
endforeach()

project(Talas VERSION 0.1.0 LANGUAGES C CXX)

option(TALAS_BUILD_TESTS "Build Talas tests" ON)
option(TALAS_BUILD_WORKBENCH "Build the JUCE standalone workbench" OFF)
option(TALAS_FETCH_JUCE "Fetch the pinned JUCE dependency for the Workbench" OFF)
option(TALAS_ENABLE_DMG_PACKAGE "Also generate an unsigned macOS DMG" OFF)
option(TALAS_BUILD_FUZZERS "Build opt-in Clang sanitizer fuzz harnesses" OFF)
option(TALAS_BUILD_BENCHMARKS "Build opt-in core performance benchmarks" OFF)
option(TALAS_BUILD_PRODUCT_BENCHMARKS
    "Build opt-in JUCE product performance probes" OFF)
set(TALAS_BENCHMARK_PRESET_ID "" CACHE STRING
    "Verified configure preset identity for performance reports")
set(TALAS_FUZZ_ENGINE "REPLAY" CACHE STRING
    "Fuzz engine: REPLAY or LIBFUZZER")
set_property(CACHE TALAS_FUZZ_ENGINE PROPERTY STRINGS REPLAY LIBFUZZER)
option(TALAS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(TALAS_JUCE_PATH "" CACHE PATH "Path to a JUCE source checkout")
set(TALAS_JUCE_VERSION "8.0.15")
set(TALAS_JUCE_GIT_REVISION "91ad83ae34a81e0833b1a2b0866f54846370ae53")

# Warnings stay on for every build so new defects are visible immediately.
# Partial aggregate initialization is an intentional project idiom.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -Wno-missing-field-initializers)
    if(TALAS_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
        # JUCE sources compile inside the Workbench targets, and the pinned
        # JUCE release still calls APIs that newer SDKs deprecate.
        if(TALAS_BUILD_WORKBENCH)
            add_compile_options(-Wno-error=deprecated-declarations)
        endif()
    endif()
elseif(MSVC AND TALAS_WARNINGS_AS_ERRORS)
    add_compile_options(/WX)
endif()

if(TALAS_FETCH_JUCE AND NOT TALAS_BUILD_WORKBENCH)
    message(FATAL_ERROR "TALAS_FETCH_JUCE requires TALAS_BUILD_WORKBENCH=ON")
endif()

if(TALAS_BUILD_FUZZERS AND NOT TALAS_BUILD_TESTS)
    message(FATAL_ERROR "TALAS_BUILD_FUZZERS requires TALAS_BUILD_TESTS=ON")
endif()

if(TALAS_BUILD_BENCHMARKS AND NOT TALAS_BUILD_TESTS)
    message(FATAL_ERROR "TALAS_BUILD_BENCHMARKS requires TALAS_BUILD_TESTS=ON")
endif()

if(TALAS_BUILD_PRODUCT_BENCHMARKS
    AND (NOT TALAS_BUILD_TESTS OR NOT TALAS_BUILD_WORKBENCH))
    message(FATAL_ERROR
        "TALAS_BUILD_PRODUCT_BENCHMARKS requires TALAS_BUILD_TESTS=ON and TALAS_BUILD_WORKBENCH=ON")
endif()

if(TALAS_BUILD_PRODUCT_BENCHMARKS
    AND NOT TALAS_BENCHMARK_PRESET_ID STREQUAL "product-bench-release")
    message(FATAL_ERROR
        "TALAS_BUILD_PRODUCT_BENCHMARKS requires TALAS_BENCHMARK_PRESET_ID=product-bench-release")
endif()

if(TALAS_BUILD_PRODUCT_BENCHMARKS
    AND (NOT CMAKE_BUILD_TYPE STREQUAL "Release"
         OR TALAS_FETCH_JUCE
         OR TALAS_JUCE_PATH STREQUAL ""))
    message(FATAL_ERROR
        "TALAS_BUILD_PRODUCT_BENCHMARKS requires Release and explicit local JUCE")
endif()

if(TALAS_BUILD_BENCHMARKS
    AND NOT TALAS_BENCHMARK_PRESET_ID STREQUAL "core-bench-release")
    message(FATAL_ERROR
        "TALAS_BUILD_BENCHMARKS requires TALAS_BENCHMARK_PRESET_ID=core-bench-release")
endif()

if(TALAS_BUILD_BENCHMARKS OR TALAS_BUILD_PRODUCT_BENCHMARKS)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL
        "Export commands for the benchmark build manifest" FORCE)
endif()

if(TALAS_BUILD_TESTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()

if(TALAS_BUILD_PRODUCT_BENCHMARKS)
    if(NOT Python3_Interpreter_FOUND)
        message(FATAL_ERROR
            "Product performance probe compiler capture requires Python 3")
    endif()
    get_filename_component(TALAS_PRODUCT_PROBE_JUCE_ROOT
        "${TALAS_JUCE_PATH}" REALPATH BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    execute_process(
        COMMAND git rev-parse --verify HEAD
        WORKING_DIRECTORY "${TALAS_PRODUCT_PROBE_JUCE_ROOT}"
        OUTPUT_VARIABLE TALAS_PRODUCT_PROBE_JUCE_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE TALAS_PRODUCT_PROBE_JUCE_GIT_RESULT)
    execute_process(
        COMMAND git diff --quiet HEAD --
        WORKING_DIRECTORY "${TALAS_PRODUCT_PROBE_JUCE_ROOT}"
        RESULT_VARIABLE TALAS_PRODUCT_PROBE_JUCE_DIRTY_RESULT)
    if(NOT TALAS_PRODUCT_PROBE_JUCE_GIT_RESULT EQUAL 0
        OR NOT TALAS_PRODUCT_PROBE_JUCE_COMMIT STREQUAL TALAS_JUCE_GIT_REVISION
        OR NOT TALAS_PRODUCT_PROBE_JUCE_DIRTY_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Product performance probe requires clean pinned local JUCE")
    endif()
    execute_process(
        COMMAND git rev-parse --verify HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE TALAS_PRODUCT_PROBE_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE TALAS_PRODUCT_PROBE_GIT_RESULT)
    string(LENGTH "${TALAS_PRODUCT_PROBE_COMMIT}"
        TALAS_PRODUCT_PROBE_COMMIT_LENGTH)
    execute_process(
        COMMAND git diff --quiet HEAD --
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        RESULT_VARIABLE TALAS_PRODUCT_PROBE_DIRTY_RESULT)
    if(NOT TALAS_PRODUCT_PROBE_GIT_RESULT EQUAL 0
        OR NOT TALAS_PRODUCT_PROBE_COMMIT_LENGTH EQUAL 40
        OR NOT TALAS_PRODUCT_PROBE_COMMIT MATCHES "^[0-9a-f]+$"
        OR NOT TALAS_PRODUCT_PROBE_DIRTY_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Product performance probe requires a clean exact Git commit")
    endif()
    set(TALAS_PRODUCT_PROBE_COMPILER_INVOCATION_LOG_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/talas-product-probe-compiler-invocations")
    set(TALAS_PRODUCT_PROBE_COMPILER_CONTRACT
        "${CMAKE_CURRENT_BINARY_DIR}/talas-product-probe-compiler-contract.json")
    file(WRITE "${TALAS_PRODUCT_PROBE_COMPILER_CONTRACT}"
        "{\n"
        "  \"format\": \"talas.product-probe-compiler-contract\",\n"
        "  \"version\": 2,\n"
        "  \"sourceRoot\": \"${CMAKE_CURRENT_SOURCE_DIR}\",\n"
        "  \"commit\": \"${TALAS_PRODUCT_PROBE_COMMIT}\",\n"
        "  \"juceRoot\": \"${TALAS_PRODUCT_PROBE_JUCE_ROOT}\",\n"
        "  \"juceCommit\": \"${TALAS_PRODUCT_PROBE_JUCE_COMMIT}\",\n"
        "  \"logDir\": \"${TALAS_PRODUCT_PROBE_COMPILER_INVOCATION_LOG_DIR}\"\n"
        "}\n")
    set(CMAKE_C_COMPILER_LAUNCHER
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/capture_product_probe_compiler.py")
    set(CMAKE_CXX_COMPILER_LAUNCHER
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/capture_product_probe_compiler.py")
endif()

if(TALAS_BUILD_BENCHMARKS)
    if(NOT Python3_Interpreter_FOUND)
        message(FATAL_ERROR
            "Core performance benchmark compiler capture requires Python 3")
    endif()
    set(TALAS_BENCHMARK_COMPILER_INVOCATION_LOG_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/talas-performance-compiler-invocations")
    set(TALAS_BENCHMARK_COMPILER_LAUNCHER
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/capture_compiler_invocation.py"
        "--log-dir"
        "${TALAS_BENCHMARK_COMPILER_INVOCATION_LOG_DIR}"
        "--")
    set(CMAKE_C_COMPILER_LAUNCHER ${TALAS_BENCHMARK_COMPILER_LAUNCHER})
    set(CMAKE_CXX_COMPILER_LAUNCHER ${TALAS_BENCHMARK_COMPILER_LAUNCHER})
endif()

if(TALAS_BUILD_TESTS OR TALAS_BUILD_WORKBENCH)
    enable_testing()
endif()

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(talas_core
    src/core/BuiltinNodeBindings.cpp
    src/core/BuiltinNodes.cpp
    src/core/Graph.cpp
    src/core/NodeCatalogJson.cpp
    src/core/NodeRegistry.cpp
    src/core/StaticResourceList.cpp
    src/expr/Expression.cpp
)

target_include_directories(talas_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(talas_core PUBLIC cxx_std_20)

add_library(talas_generative
    src/generative/ContextFreeGenerator.cpp
    src/generative/TalasGrammar.cpp
)

target_include_directories(talas_generative
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(talas_generative PUBLIC cxx_std_20)

add_library(talas_dsp
    src/dsp/DiffuseSpectralStretch.cpp
    src/dsp/IncrementalComplexEigenvalues.cpp
    src/dsp/IncrementalForwardBackwardSubspace.cpp
    src/dsp/IncrementalLagSubspace.cpp
    src/dsp/IncrementalSinusoidFit.cpp
    src/dsp/IncrementalStationaryModel.cpp
    src/dsp/IncrementalSubspaceModes.cpp
    src/dsp/LinearVocoderBank.cpp
    src/dsp/PreparedGranularScheduler.cpp
    src/dsp/PreparedSpectrumFilterBank.cpp
    src/dsp/ScaleVocoderIntervals.cpp
    src/dsp/SerializedSpectrumFrameAssembler.cpp
    src/dsp/SerializedSpectrumFrameSynchronizer.cpp
    src/dsp/VocoderBandShaping.cpp
    src/dsp/LiveSpectralAnalysis.cpp
    src/dsp/NativeStft.cpp
    src/dsp/PackedRealFftAdapter.cpp
    src/dsp/FrequencyScaleGranulator.cpp
    src/dsp/PhaseVocoderPitchShifter.cpp
    src/dsp/PreparedTimeFrequencyScale.cpp
    src/dsp/Radix2Fft.cpp
    src/dsp/ReAnalysis.cpp
    src/dsp/SpectralFrame.cpp
    src/dsp/SpectralWindow.cpp
    src/dsp/SystemWavetables.cpp
)

target_include_directories(talas_dsp
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(talas_dsp PUBLIC cxx_std_20)

# Large source-analysis transforms also run on the audio thread in Debug.
# Preserve symbols without enabling fast-math while optimizing these two source
# files (including standard FFT consumers). An unoptimized 8192-frame transform
# exceeds a 128/48k callback. The compiler's usual FP-contraction policy remains.
set_property(SOURCE src/dsp/PreparedTimeFrequencyScale.cpp src/dsp/Radix2Fft.cpp
    APPEND PROPERTY COMPILE_OPTIONS
    "$<$<AND:$<CONFIG:Debug>,$<CXX_COMPILER_ID:AppleClang,Clang,GNU>>:-O2>")

# CDP-derived algorithms stay in their own LGPL-2.1 library (ADR-0018).
add_library(talas_cdp
    third_party/cdp/src/DistortEnvelope.cpp
    third_party/cdp/src/DistortHarmonic.cpp
    third_party/cdp/src/DistortMultiply.cpp
    third_party/cdp/src/DistortOmit.cpp
    third_party/cdp/src/DistortOverload.cpp
    third_party/cdp/src/DistortReform.cpp
    third_party/cdp/src/DistortReverse.cpp
    third_party/cdp/src/DistortShuffle.cpp
    third_party/cdp/src/Drand48.cpp
    third_party/cdp/src/Echo.cpp
    third_party/cdp/src/FixedFilter.cpp
    third_party/cdp/src/LohiFilter.cpp
    third_party/cdp/src/LowerResolution.cpp
    third_party/cdp/src/Pan.cpp
    third_party/cdp/src/PhaseVocoder.cpp
    third_party/cdp/src/Phasing.cpp
    third_party/cdp/src/Reverb.cpp
    third_party/cdp/src/RingModulate.cpp
    third_party/cdp/src/SpectralBlur.cpp
    third_party/cdp/src/SpectralChorus.cpp
    third_party/cdp/src/SpectralFocus.cpp
    third_party/cdp/src/SpectralPitch.cpp
    third_party/cdp/src/SpectralScatter.cpp
    third_party/cdp/src/SpectralArpeg.cpp
    third_party/cdp/src/SpectralBands.cpp
    third_party/cdp/src/SpectralEnvelope.cpp
    third_party/cdp/src/SpectralGlis.cpp
    third_party/cdp/src/SpectralCombine.cpp
    third_party/cdp/src/SpectralMorph.cpp
    third_party/cdp/src/SpectralTransposef.cpp
    third_party/cdp/src/SpectralFormants.cpp
    third_party/cdp/src/SpectralBlurBlur.cpp
    third_party/cdp/src/SpectralFocusPeaks.cpp
    third_party/cdp/src/SpectralPitchFile.cpp
    third_party/cdp/src/PitchTrack.cpp
    third_party/cdp/src/SpectralFreeze.cpp
    third_party/cdp/src/SpectralWholeSound.cpp
    third_party/cdp/src/SpectralWholeFile.cpp
    third_party/cdp/src/SpectralWholeFileBlur.cpp
    third_party/cdp/src/SpectralWholeFileSpec.cpp
    third_party/cdp/src/SpectralWholeFileStretch.cpp
    third_party/cdp/src/ChannelTools.cpp
    third_party/cdp/src/EnvelopeWarpStretch.cpp
    third_party/cdp/src/MergeMany.cpp
    third_party/cdp/src/Prefix.cpp
    third_party/cdp/src/SilenceMasks.cpp
    third_party/cdp/src/StereoPlacement.cpp
    third_party/cdp/src/StereoSpace.cpp
    third_party/cdp/src/VaryingBank2.cpp
    third_party/cdp/src/TremoloFine.cpp
    third_party/cdp/src/WavesetLocal.cpp
    third_party/cdp/src/RoomReverb.cpp
    third_party/cdp/src/Quantise.cpp
    third_party/cdp/src/LoudnessGain.cpp
    third_party/cdp/src/PhaseInvert.cpp
    third_party/cdp/src/DcShift.cpp
    third_party/cdp/src/TwoInput.cpp
    third_party/cdp/src/FastConvolution.cpp
    third_party/cdp/src/SilenceGate.cpp
    third_party/cdp/src/Deglitch.cpp
    third_party/cdp/src/NoiseCut.cpp
    third_party/cdp/src/AttackBoost.cpp
    third_party/cdp/src/SubmixPairs.cpp
    third_party/cdp/src/EnvelopeWarp.cpp
    third_party/cdp/src/PitchedDelay.cpp
    third_party/cdp/src/Bounce.cpp
    third_party/cdp/src/Rejoin.cpp
    third_party/cdp/src/Cascade.cpp
    third_party/cdp/src/Phasor.cpp
    third_party/cdp/src/Spin.cpp
    third_party/cdp/src/Brassage.cpp
    third_party/cdp/src/RotatingPan.cpp
    third_party/cdp/src/SpreadPan.cpp
    third_party/cdp/src/TablePan.cpp
    third_party/cdp/src/StepSpread.cpp
    third_party/cdp/src/CrossPan.cpp
    third_party/cdp/src/Antiphony.cpp
    third_party/cdp/src/MultichannelStadium.cpp
    third_party/cdp/src/Flutter.cpp
    third_party/cdp/src/FrameRotate.cpp
    third_party/cdp/src/FrameRemap.cpp
    third_party/cdp/src/ChannelEnvelope.cpp
    third_party/cdp/src/ToStereo.cpp
    third_party/cdp/src/SpectralSimple.cpp
    third_party/cdp/src/StadiumEcho.cpp
    third_party/cdp/src/SweepingFilter.cpp
    third_party/cdp/src/TapDelay.cpp
    third_party/cdp/src/Tremolo.cpp
    third_party/cdp/src/UserBank.cpp
    third_party/cdp/src/VariableFilter.cpp
    third_party/cdp/src/VaryingBank.cpp
    third_party/cdp/src/VaryingDelay.cpp
)

target_include_directories(talas_cdp
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cdp/include
)

target_compile_features(talas_cdp PUBLIC cxx_std_20)

add_library(talas_project
    src/project/PartialTrackAsset.cpp
    src/project/SosSpectrumFile.cpp
    src/project/PartialTrackAnalysis.cpp
    src/project/ProjectPartialTrackEdits.cpp
    src/project/ControllerRecording.cpp
    src/project/JsonValue.cpp
    src/project/ProjectAutosaveState.cpp
    src/project/ProjectAssetInspection.cpp
    src/project/ProjectAssetPaths.cpp
    src/project/ProjectAssetResolver.cpp
    src/project/ProjectDocument.cpp
    src/project/ProjectDiskCache.cpp
    src/project/ProjectEditor.cpp
    src/project/ProjectExpressionAudit.cpp
    src/project/ProjectEventValueDiscovery.cpp
    src/project/ProjectEventValueSpelling.cpp
    src/project/ProjectFile.cpp
    src/project/ProjectGenerators.cpp
    src/project/ProjectGraphClipboard.cpp
    src/project/ProjectLiveSets.cpp
    src/project/ProjectLocalMapControlDiscovery.cpp
    src/project/ProjectGeneratorOutputs.cpp
    src/project/ProjectParameterTransformer.cpp
    src/project/SmalltalkExactNumber.cpp
    src/project/SmalltalkSubset.cpp
    src/project/Tool.cpp
    src/project/ToolFile.cpp
    src/project/PreparedProjectGenerators.cpp
    src/project/ProjectJson.cpp
    src/project/LegacyMixerMigration.cpp
    src/project/ProjectMissingNodes.cpp
    src/project/ProjectMusicalTime.cpp
    src/project/ProjectMidiVoiceIslands.cpp
    src/project/ProjectGlobalMap.cpp
    src/project/AlternateTunings.cpp
    src/project/ProjectMidiMapperMap.cpp
    src/project/ProjectPackage.cpp
    src/project/PreparedProjectSoundVariables.cpp
    src/project/PreparedProjectVariables.cpp
    src/project/ProjectSoundExpansion.cpp
    src/project/ProjectStructureText.cpp
    src/project/ProjectSnapshots.cpp
    src/project/ProjectSoundVariables.cpp
    src/project/ProjectValidation.cpp
    src/project/ProjectVariables.cpp
    src/project/VariableLiterals.cpp
    src/project/RecentProjectList.cpp
    src/project/ProjectRecoveryFile.cpp
    src/project/ProjectScore.cpp
    src/project/ProjectScoreClipFingerprint.cpp
    src/project/SemanticVersion.cpp
    src/project/SoundDefinitionImport.cpp
    src/project/SpeakerPlacement.cpp
    src/project/ProjectSampleMaps.cpp
    src/project/StatePackageSchemas.cpp
    src/project/TimelineInteraction.cpp
    src/project/ProjectEditBoundary.cpp
    src/project/VirtualControlMidi.cpp
    src/project/VirtualControlOsc.cpp
    src/project/WorkspaceSessionFile.cpp
    src/project/WorkspaceSessionState.cpp
)

target_include_directories(talas_project
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(talas_project PUBLIC talas_core talas_generative)

target_compile_features(talas_project PUBLIC cxx_std_20)

add_library(talas_component_isolation_support
    src/runtime/ComponentIsolationProductAudioWorker.cpp
    src/runtime/ComponentIsolationControlSession.cpp
    src/runtime/ComponentIsolationProductHelperSession.cpp
    src/runtime/ComponentIsolationProtocol.cpp
)

target_include_directories(talas_component_isolation_support
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(talas_component_isolation_support PUBLIC cxx_std_20)

add_library(talas_component_activation_protocol STATIC
    src/runtime/component_activation/ComponentActivationProtocol.cpp
)

target_include_directories(talas_component_activation_protocol
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(talas_component_activation_protocol PUBLIC talas_project)
target_compile_features(talas_component_activation_protocol PUBLIC cxx_std_20)

add_library(talas_pcm16_wave STATIC
    src/audio/Pcm16WaveSpan.cpp
)

target_include_directories(talas_pcm16_wave
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(talas_pcm16_wave PUBLIC cxx_std_20)

add_library(talas_component_activation_audio_source STATIC
    src/runtime/component_activation/ComponentActivationAudioSourceFreezer.cpp
)

target_include_directories(talas_component_activation_audio_source
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(talas_component_activation_audio_source
    PUBLIC talas_component_activation_protocol
    PRIVATE talas_pcm16_wave)
target_compile_features(
    talas_component_activation_audio_source PUBLIC cxx_std_20)

add_library(talas_component_activation_audio_stream STATIC
    src/runtime/component_activation/ComponentActivationAudioStream.cpp
)

target_include_directories(talas_component_activation_audio_stream
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(talas_component_activation_audio_stream
    PUBLIC talas_component_activation_protocol
    PRIVATE talas_pcm16_wave)
target_compile_features(
    talas_component_activation_audio_stream PUBLIC cxx_std_20)
if(TALAS_BUILD_TESTS)
    target_compile_definitions(
        talas_component_activation_audio_stream
        PUBLIC TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT=1)
endif()

add_library(talas_component_activation_authorization STATIC
    src/runtime/component_activation/ComponentActivationAuthorization.cpp
)

target_include_directories(talas_component_activation_authorization
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(talas_component_activation_authorization
    PUBLIC talas_component_activation_audio_source talas_runtime)
target_compile_features(
    talas_component_activation_authorization PUBLIC cxx_std_20)

if(APPLE)
    set(TALAS_COMPONENT_ISOLATION_HELPER_ENTITLEMENTS
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/TalasComponentHost.entitlements")
    set(TALAS_COMPONENT_PROBE_HELPER_ENTITLEMENTS
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/TalasComponentProbe.entitlements")
    set(TALAS_COMPONENT_RUNTIME_HELPER_ENTITLEMENTS
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/TalasComponentRuntime.entitlements")

    function(talas_sign_component_isolation_helper target_name)
        add_custom_command(
            TARGET ${target_name}
            POST_BUILD
            COMMAND /usr/bin/codesign
                --force
                --sign -
                --timestamp=none
                --entitlements
                    "${TALAS_COMPONENT_ISOLATION_HELPER_ENTITLEMENTS}"
                "$<TARGET_BUNDLE_DIR:${target_name}>"
            COMMAND /usr/bin/codesign
                --verify
                --strict
                "$<TARGET_BUNDLE_DIR:${target_name}>"
            VERBATIM)
    endfunction()

    function(talas_sign_component_probe_helper target_name)
        add_custom_command(
            TARGET ${target_name}
            POST_BUILD
            COMMAND /usr/bin/codesign
                --force
                --sign -
                --timestamp=none
                --entitlements
                    "${TALAS_COMPONENT_PROBE_HELPER_ENTITLEMENTS}"
                "$<TARGET_BUNDLE_DIR:${target_name}>"
            COMMAND /usr/bin/codesign
                --verify
                --strict
                "$<TARGET_BUNDLE_DIR:${target_name}>"
            VERBATIM)
    endfunction()

    function(talas_sign_component_activation_helper target_name)
        add_custom_command(
            TARGET ${target_name}
            POST_BUILD
            COMMAND /usr/bin/codesign
                --force
                --sign -
                --timestamp=none
                --entitlements
                    "${TALAS_COMPONENT_RUNTIME_HELPER_ENTITLEMENTS}"
                "$<TARGET_BUNDLE_DIR:${target_name}>"
            COMMAND /usr/bin/codesign
                --verify
                --strict
                "$<TARGET_BUNDLE_DIR:${target_name}>"
            VERBATIM)
    endfunction()

    add_library(talas_component_isolation_macos_xpc STATIC
        src/runtime/macos/ComponentIsolationXpcEnvelope.cpp
    )
    target_include_directories(talas_component_isolation_macos_xpc
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(
        talas_component_isolation_macos_xpc
        PUBLIC talas_component_isolation_support)
    target_compile_features(talas_component_isolation_macos_xpc PUBLIC cxx_std_20)

    add_library(talas_component_probe_macos_xpc STATIC
        src/runtime/component_probe/macos/ComponentBundleProbeXpcEnvelope.cpp
    )
    target_include_directories(talas_component_probe_macos_xpc
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(talas_component_probe_macos_xpc PUBLIC talas_runtime)
    target_compile_features(talas_component_probe_macos_xpc PUBLIC cxx_std_20)

    add_library(talas_component_activation_macos_xpc STATIC
        src/runtime/component_activation/macos/ComponentActivationXpcEnvelope.cpp
    )
    target_include_directories(talas_component_activation_macos_xpc
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(talas_component_activation_macos_xpc
        PUBLIC talas_runtime
        PRIVATE talas_component_activation_audio_source)
    target_compile_features(
        talas_component_activation_macos_xpc PUBLIC cxx_std_20)
    add_library(talas_component_activation_loader STATIC
        src/runtime/component_activation/macos/ComponentActivationLoader.cpp
    )
    target_include_directories(talas_component_activation_loader
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(talas_component_activation_loader
        PRIVATE
            talas_component_probe_validation
            talas_component_activation_audio_stream)
    target_compile_features(talas_component_activation_loader PUBLIC cxx_std_20)

    add_library(talas_component_activation_worker STATIC
        src/runtime/component_activation/ComponentActivationWorker.cpp)
    target_include_directories(talas_component_activation_worker
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(talas_component_activation_worker
        PUBLIC talas_component_activation_loader
        PRIVATE talas_runtime)
    target_compile_features(
        talas_component_activation_worker PUBLIC cxx_std_20)

    add_library(talas_component_activation_macos_host STATIC
        src/runtime/component_activation/macos/ComponentActivationMacOSHost.cpp
    )
    target_include_directories(talas_component_activation_macos_host
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(talas_component_activation_macos_host
        PUBLIC talas_component_activation_macos_xpc
        PRIVATE talas_runtime talas_component_activation_authorization)
    target_compile_features(
        talas_component_activation_macos_host PUBLIC cxx_std_20)
    if(TALAS_BUILD_TESTS)
        target_compile_definitions(
            talas_component_activation_macos_host
            PUBLIC
                TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT=1)
    endif()
    target_compile_options(
        talas_component_activation_macos_host PRIVATE -fblocks)

    add_library(talas_component_probe_macos_materialization STATIC
        src/runtime/component_probe/macos/ComponentBundleProbeMaterialization.cpp
    )
    target_include_directories(talas_component_probe_macos_materialization
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(talas_component_probe_macos_materialization
        PUBLIC talas_runtime)
    target_compile_features(
        talas_component_probe_macos_materialization
        PUBLIC cxx_std_20)

    add_library(talas_component_probe_macos_coordinator STATIC
        src/runtime/component_probe/macos/ComponentBundleProbeMacOSCoordinator.cpp
    )
    target_include_directories(talas_component_probe_macos_coordinator
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(talas_component_probe_macos_coordinator
        PUBLIC talas_component_probe_macos_xpc
        PRIVATE
            talas_component_probe_macos_materialization
            "-framework Security")
    target_compile_features(
        talas_component_probe_macos_coordinator
        PUBLIC cxx_std_20)
    target_compile_options(
        talas_component_probe_macos_coordinator
        PRIVATE -fblocks)

    add_library(talas_component_bundle_replacement_controller STATIC
        src/runtime/component_bundle/ComponentBundleReplacementController.cpp
    )
    target_include_directories(
        talas_component_bundle_replacement_controller
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(
        talas_component_bundle_replacement_controller
        PUBLIC talas_runtime
        PRIVATE
            talas_component_activation_authorization
            talas_component_activation_macos_host
            talas_component_probe_macos_coordinator)
    target_compile_features(
        talas_component_bundle_replacement_controller PUBLIC cxx_std_20)
    target_compile_definitions(
        talas_component_bundle_replacement_controller
        PUBLIC TALAS_COMPONENT_BUNDLE_REPLACEMENT_SUPPORT=1)
    if(TALAS_BUILD_TESTS)
        target_compile_definitions(
            talas_component_bundle_replacement_controller
            PUBLIC TALAS_COMPONENT_BUNDLE_REPLACEMENT_TEST_SUPPORT=1)
    endif()

    add_executable(talas_component_bundle_probe_helper MACOSX_BUNDLE
        src/runtime/component_probe/macos/ComponentBundleProbeHelperMain.cpp
        src/runtime/component_probe/macos/ComponentBundleProbeLoader.cpp
    )
    target_include_directories(talas_component_bundle_probe_helper
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(talas_component_bundle_probe_helper
        PRIVATE
            talas_component_probe_macos_xpc
            talas_component_probe_validation
            talas_component_probe_macos_materialization)
    target_compile_features(
        talas_component_bundle_probe_helper
        PRIVATE cxx_std_20)
    target_compile_options(
        talas_component_bundle_probe_helper
        PRIVATE -fblocks)
    set_target_properties(talas_component_bundle_probe_helper PROPERTIES
        OUTPUT_NAME TalasComponentProbe
        BUNDLE_EXTENSION xpc
        MACOSX_BUNDLE_INFO_PLIST
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/TalasComponentProbe-Info.plist"
        MACOSX_BUNDLE_GUI_IDENTIFIER
            "com.talas.workbench.component-probe"
        MACOSX_BUNDLE_BUNDLE_NAME "Talas Component Probe"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
    talas_sign_component_probe_helper(talas_component_bundle_probe_helper)

    add_executable(talas_component_activation_helper MACOSX_BUNDLE
        src/runtime/component_activation/macos/ComponentActivationHelperMain.cpp
    )
    target_include_directories(talas_component_activation_helper
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(talas_component_activation_helper
        PRIVATE
            talas_component_activation_macos_xpc
            talas_component_activation_loader
            talas_component_activation_worker
            talas_component_probe_validation)
    target_compile_features(
        talas_component_activation_helper PRIVATE cxx_std_20)
    target_compile_options(
        talas_component_activation_helper PRIVATE -fblocks)
    set_target_properties(talas_component_activation_helper PROPERTIES
        OUTPUT_NAME TalasComponentRuntime
        BUNDLE_EXTENSION xpc
        MACOSX_BUNDLE_INFO_PLIST
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/TalasComponentRuntime-Info.plist"
        MACOSX_BUNDLE_GUI_IDENTIFIER
            "com.talas.workbench.component-runtime"
        MACOSX_BUNDLE_BUNDLE_NAME "Talas Component Runtime"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
    talas_sign_component_activation_helper(talas_component_activation_helper)

    if(TALAS_BUILD_TESTS)
        function(talas_add_component_activation_fixture_helper
            target_name output_name bundle_identifier fixture_mode)
            set(TALAS_COMPONENT_RUNTIME_EXECUTABLE "${output_name}")
            set(TALAS_COMPONENT_RUNTIME_BUNDLE_IDENTIFIER "${bundle_identifier}")
            configure_file(
                "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/TalasComponentRuntimeFixture-Info.plist.in"
                "${CMAKE_CURRENT_BINARY_DIR}/${target_name}-Info.plist"
                @ONLY)
            add_executable(${target_name} MACOSX_BUNDLE
                src/runtime/component_activation/macos/ComponentActivationHelperMain.cpp)
            target_include_directories(${target_name}
                PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
            target_link_libraries(${target_name}
                PRIVATE talas_component_activation_macos_xpc)
            target_compile_features(${target_name} PRIVATE cxx_std_20)
            target_compile_options(${target_name} PRIVATE -fblocks)
            target_compile_definitions(${target_name}
                PRIVATE TALAS_COMPONENT_ACTIVATION_HELPER_FIXTURE_MODE=${fixture_mode})
            set_target_properties(${target_name} PROPERTIES
                OUTPUT_NAME "${output_name}"
                BUNDLE_EXTENSION xpc
                MACOSX_BUNDLE_INFO_PLIST
                    "${CMAKE_CURRENT_BINARY_DIR}/${target_name}-Info.plist")
            talas_sign_component_activation_helper(${target_name})
        endfunction()

        talas_add_component_activation_fixture_helper(
            talas_component_activation_helper_malformed
            TalasComponentRuntimeMalformed
            com.talas.workbench.component-runtime.fixture.malformed
            1)
        talas_add_component_activation_fixture_helper(
            talas_component_activation_helper_hang
            TalasComponentRuntimeHang
            com.talas.workbench.component-runtime.fixture.hang
            2)
        talas_add_component_activation_fixture_helper(
            talas_component_activation_helper_session_mismatch
            TalasComponentRuntimeSessionMismatch
            com.talas.workbench.component-runtime.fixture.mismatch
            3)
        talas_add_component_activation_fixture_helper(
            talas_component_activation_helper_heartbeat_hang
            TalasComponentRuntimeHeartbeatHang
            com.talas.workbench.component-runtime.fixture.heartbeat-hang
            4)
    endif()

    add_library(talas_component_isolation_macos_host STATIC
        src/runtime/macos/ComponentIsolationMacOSHost.cpp
    )
    target_include_directories(talas_component_isolation_macos_host
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(
        talas_component_isolation_macos_host
        PUBLIC talas_component_isolation_macos_xpc
        PRIVATE talas_runtime)
    target_compile_features(
        talas_component_isolation_macos_host
        PUBLIC cxx_std_20)
    target_compile_options(
        talas_component_isolation_macos_host
        PRIVATE -fblocks)

    add_executable(talas_component_isolation_helper MACOSX_BUNDLE
        src/runtime/macos/ComponentIsolationHelperMain.cpp
    )
    target_include_directories(talas_component_isolation_helper
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(
        talas_component_isolation_helper
        PRIVATE
            talas_component_isolation_macos_xpc)
    target_compile_features(
        talas_component_isolation_helper
        PRIVATE cxx_std_20)
    target_compile_options(
        talas_component_isolation_helper
        PRIVATE -fblocks)
    set_target_properties(talas_component_isolation_helper PROPERTIES
        OUTPUT_NAME "TalasComponentHost"
        BUNDLE_EXTENSION "xpc"
        MACOSX_BUNDLE_INFO_PLIST
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/TalasComponentHost-Info.plist"
        MACOSX_BUNDLE_GUI_IDENTIFIER
            "com.talas.workbench.component-host"
        MACOSX_BUNDLE_BUNDLE_NAME "Talas Component Host"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
    talas_sign_component_isolation_helper(talas_component_isolation_helper)

    if(TALAS_BUILD_TESTS)
        function(talas_add_component_isolation_fixture_helper
            target_name output_name bundle_identifier fixture_mode)
            set(TALAS_COMPONENT_HELPER_EXECUTABLE "${output_name}")
            set(TALAS_COMPONENT_HELPER_BUNDLE_IDENTIFIER "${bundle_identifier}")
            configure_file(
                "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/TalasComponentHostFixture-Info.plist.in"
                "${CMAKE_CURRENT_BINARY_DIR}/${target_name}-Info.plist"
                @ONLY)
            add_executable(${target_name} MACOSX_BUNDLE
                src/runtime/macos/ComponentIsolationHelperMain.cpp)
            target_include_directories(${target_name}
                PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
            target_link_libraries(${target_name}
                PRIVATE talas_component_isolation_macos_xpc)
            target_compile_features(${target_name} PRIVATE cxx_std_20)
            target_compile_options(${target_name} PRIVATE -fblocks)
            target_compile_definitions(${target_name}
                PRIVATE
                    TALAS_COMPONENT_ISOLATION_HELPER_FIXTURE_MODE=${fixture_mode})
            set_target_properties(${target_name} PROPERTIES
                OUTPUT_NAME "${output_name}"
                BUNDLE_EXTENSION "xpc"
                MACOSX_BUNDLE_INFO_PLIST
                    "${CMAKE_CURRENT_BINARY_DIR}/${target_name}-Info.plist")
            talas_sign_component_isolation_helper(${target_name})
        endfunction()

        talas_add_component_isolation_fixture_helper(
            talas_component_isolation_helper_malformed
            TalasComponentHostMalformed
            com.talas.workbench.component-host.fixture.malformed
            1)
        talas_add_component_isolation_fixture_helper(
            talas_component_isolation_helper_wrong_version
            TalasComponentHostWrongVersion
            com.talas.workbench.component-host.fixture.wrong-version
            2)
        talas_add_component_isolation_fixture_helper(
            talas_component_isolation_helper_wrong_nonce
            TalasComponentHostWrongNonce
            com.talas.workbench.component-host.fixture.wrong-nonce
            3)
        talas_add_component_isolation_fixture_helper(
            talas_component_isolation_helper_stale_generation
            TalasComponentHostStaleGeneration
            com.talas.workbench.component-host.fixture.stale-generation
            4)
        talas_add_component_isolation_fixture_helper(
            talas_component_isolation_helper_controlled_failure
            TalasComponentHostControlledFailure
            com.talas.workbench.component-host.fixture.failure
            5)
        talas_add_component_isolation_fixture_helper(
            talas_component_isolation_helper_crash
            TalasComponentHostCrash
            com.talas.workbench.component-host.fixture.crash
            6)
        talas_add_component_isolation_fixture_helper(
            talas_component_isolation_helper_hang
            TalasComponentHostHang
            com.talas.workbench.component-host.fixture.hang
            7)
        talas_add_component_isolation_fixture_helper(
            talas_component_isolation_helper_oversized_diagnostic
            TalasComponentHostOversizedDiagnostic
            com.talas.workbench.component-host.fixture.oversized
            8)
        talas_add_component_isolation_fixture_helper(
            talas_component_isolation_helper_early_exit
            TalasComponentHostEarlyExit
            com.talas.workbench.component-host.fixture.early-exit
            9)
        talas_add_component_isolation_fixture_helper(
            talas_component_isolation_helper_teardown_race
            TalasComponentHostTeardownRace
            com.talas.workbench.component-host.fixture.teardown-race
            10)
        talas_add_component_isolation_fixture_helper(
            talas_component_isolation_helper_sandbox
            TalasComponentHostSandbox
            com.talas.workbench.component-host.fixture.sandbox
            11)
        target_compile_definitions(
            talas_component_isolation_helper_sandbox
            PRIVATE TALAS_COMPONENT_ISOLATION_SANDBOX_PROBE_FIXTURE=1)
        target_sources(
            talas_component_isolation_helper_sandbox
            PRIVATE tests/runtime/ComponentIsolationSandboxProbe.cpp)
        talas_add_component_isolation_fixture_helper(
            talas_component_isolation_helper_heartbeat_hang
            TalasComponentHostHeartbeatHang
            com.talas.workbench.component-host.fixture.heart-hang
            12)
        talas_add_component_isolation_fixture_helper(
            talas_component_isolation_helper_teardown_hang
            TalasComponentHostTeardownHang
            com.talas.workbench.component-host.fixture.td-hang
            13)
        talas_add_component_isolation_fixture_helper(
            talas_component_isolation_helper_heartbeat_late
            TalasComponentHostHeartbeatLate
            com.talas.workbench.component-host.fixture.heart-late
            14)
        talas_add_component_isolation_fixture_helper(
            talas_component_isolation_helper_prewarm_hang
            TalasComponentHostPrewarmHang
            com.talas.workbench.component-host.fixture.prewarm-hang
            15)
    endif()
endif()

add_library(talas_component_probe_validation STATIC
    src/runtime/component_probe/ComponentBundleMachOValidation.cpp
    src/runtime/component_probe/ComponentBundleProbeAbiValidation.cpp
)

target_include_directories(talas_component_probe_validation
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(talas_component_probe_validation PUBLIC cxx_std_20)

add_library(talas_runtime
    src/runtime/AnalysisTestInputSource.cpp
    src/runtime/AudioStream.cpp
    src/runtime/BuiltinKernelAbiModule.cpp
    src/runtime/ComponentIsolationAudioDataPlane.cpp
    src/runtime/component_activation/ComponentActivationAudioDataPlane.cpp
    src/runtime/component_activation/ComponentActivationManifestProfile.cpp
    src/runtime/component_bundle/ComponentBundleDependencyResolver.cpp
    src/runtime/component_bundle/ComponentBundleInspection.cpp
    src/runtime/component_bundle/ComponentBundleManifest.cpp
    src/runtime/component_bundle/ComponentBundleOwnerAuthorization.cpp
    src/runtime/component_bundle/ComponentBundlePreservationCompatibility.cpp
    src/runtime/component_bundle/ComponentBundleReplacementConfirmation.cpp
    src/runtime/component_bundle/ComponentBundleReplacementPolicy.cpp
    src/runtime/component_bundle/ComponentBundleSignatureVerifier.cpp
    src/runtime/component_probe/ComponentBundleProbeProtocol.cpp
    src/runtime/component_probe/ComponentBundleProbeEvidence.cpp
    src/runtime/ComponentTrustPolicy.cpp
    src/runtime/KernelAbiHost.cpp
    src/runtime/KernelAbiProbe.cpp
    src/runtime/KernelRegistry.cpp
    src/runtime/MatrixRouting.cpp
    src/runtime/MidiClockPhaseFollower.cpp
    src/runtime/MtcSmpteProtocol.cpp
    src/runtime/OscProtocol.cpp
    src/runtime/MtcRealtimeIngress.cpp
    src/runtime/MtcTransportFollower.cpp
    src/runtime/PreparedGraphSummary.cpp
    src/runtime/PreparedLiveSet.cpp
    src/runtime/PreparedLiveTools.cpp
    src/runtime/PreparedMidiMapper.cpp
    src/runtime/PreparedMidiFileVoiceSources.cpp
    src/runtime/ProcessorPlacement.cpp
    src/runtime/RealtimeLiveToolIngress.cpp
    src/runtime/ProjectRenderSettings.cpp
    src/runtime/ConstantFields.cpp
    src/runtime/PreparedScore.cpp
    src/runtime/PreparedScoreClipSource.cpp
    src/runtime/ScoreClipArtifactPublication.cpp
    src/runtime/PreparedLiveScoreSources.cpp
    src/runtime/ScoreClipMaterialization.cpp
    src/runtime/EnvelopeExtraction.cpp
    src/runtime/ProjectRuntimeBridge.cpp
    src/runtime/RealtimeAnalysisCapture.cpp
    src/runtime/RealtimeAudioEngine.cpp
    src/runtime/RealtimeControlIngress.cpp
    src/runtime/RealtimeLiveSetIngress.cpp
    src/runtime/RealtimeGraphHost.cpp
    src/runtime/RealtimeMidiVoiceAllocator.cpp
    src/runtime/RealtimeOutputRecorder.cpp
    src/runtime/RealtimeRenderSession.cpp
    src/runtime/SampleMapPlayback.cpp
    src/runtime/SnapshotPerformance.cpp
    src/runtime/Runtime.cpp
    src/runtime/StandardMidiFile.cpp
    src/runtime/RuntimeConformance.cpp
    src/runtime/TypedEvent.cpp
)

target_include_directories(talas_runtime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(
    talas_runtime
    PUBLIC
        talas_project
        talas_dsp
        talas_component_isolation_support
        talas_component_activation_protocol
        talas_pcm16_wave)
target_link_libraries(talas_runtime PUBLIC talas_cdp)

if(APPLE)
    target_link_libraries(
        talas_runtime
        PRIVATE "-framework Security" "-framework CoreFoundation")
endif()

target_compile_features(talas_runtime PUBLIC cxx_std_20)
if(TALAS_BUILD_TESTS)
    target_compile_definitions(
        talas_runtime
        PUBLIC
            TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT=1
            TALAS_CONTROLLER_RECORDING_TEST_SUPPORT=1
        PRIVATE
            TALAS_COMPONENT_BUNDLE_CONFIRMATION_TEST_SUPPORT=1)
endif()

add_library(talas_audio
    src/audio/AudioFilePublication.cpp
    src/audio/AnalyzerDisplayMapping.cpp
    src/audio/OscilloscopeAnalysis.cpp
    src/audio/PartialTrackAudioAnalysis.cpp
    src/audio/PartialTrackDerivative.cpp
    src/audio/PartialTrackResynthesis.cpp
    src/audio/QuasiHarmonicAnalysis.cpp
    src/audio/SpectrumTables.cpp
    src/audio/ProjectAssetChangeTracker.cpp
    src/audio/ProjectAssetDisposition.cpp
    src/audio/ProjectAssetLoader.cpp
    src/audio/ProjectPackager.cpp
    src/audio/CdpProcessToSample.cpp
    src/audio/GaAnalysis.cpp
    src/audio/ReAnalysisFiles.cpp
    src/audio/ResourceLoader.cpp
    src/audio/SampleFileFormats.cpp
    src/audio/SampleMapImport.cpp
    src/audio/SpectrumAnalysis.cpp
    src/audio/WaveformOverview.cpp
    src/audio/WavReader.cpp
    src/audio/WavWriter.cpp
)

target_include_directories(talas_audio
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(talas_audio PUBLIC talas_project)
target_link_libraries(talas_audio PRIVATE talas_dsp)
target_link_libraries(talas_audio PRIVATE talas_pcm16_wave)
target_link_libraries(talas_audio PUBLIC talas_cdp)

target_link_libraries(talas_runtime PRIVATE talas_audio)

target_compile_features(talas_audio PUBLIC cxx_std_20)

add_library(talas_audio_export
    src/audio/DiskCacheCapture.cpp
    src/audio/ProjectWavExporter.cpp
    src/audio/LiveTimelineRecording.cpp
    src/audio/RealtimeOutputTake.cpp
    src/audio/TimelineRecording.cpp
    src/audio/TapeRecorder.cpp
)

target_include_directories(talas_audio_export
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(talas_audio_export PUBLIC talas_audio talas_runtime)

target_compile_features(talas_audio_export PUBLIC cxx_std_20)
if(TALAS_BUILD_TESTS)
    target_compile_definitions(
        talas_audio_export
        PUBLIC TALAS_REALTIME_OUTPUT_TAKE_TEST_SUPPORT=1)
endif()

add_library(talas_cli_lib
    src/cli/Cli.cpp
)

target_include_directories(talas_cli_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(talas_cli_lib PUBLIC talas_runtime talas_audio_export)

target_compile_features(talas_cli_lib PUBLIC cxx_std_20)

add_executable(talas_cli
    src/cli/main.cpp
)

target_link_libraries(talas_cli PRIVATE talas_cli_lib)

target_compile_features(talas_cli PRIVATE cxx_std_20)

add_library(talas_app_support
    src/app/AnalysisAuditionModel.cpp
    src/app/AnalysisDiagnosticsModel.cpp
    src/app/AnalysisToolsModel.cpp
    src/app/SpectralAnalysisSteps.cpp
    src/app/SpectrumSynchronization.cpp
    src/app/DiskSplicerSegments.cpp
    src/app/DiskButton.cpp
    src/app/TalasKeyEquivalents.cpp
    src/app/SoundIcons.cpp
    src/app/AnalysisWorkbenchModel.cpp
    src/app/AssetRecoveryWorkbenchModel.cpp
    src/app/AutomationLaneEditing.cpp
    src/app/ClassEditorModel.cpp
    src/app/CompiledSounds.cpp
    src/app/DescriptorPresentationModel.cpp
    src/app/ExpressionsLibraryModel.cpp
    src/app/GeneratorPreview.cpp
    src/app/GraphBranchLayout.cpp
    src/app/GraphLineContour.cpp
    src/app/FileLocator.cpp
    src/app/SoundInformation.cpp
    src/app/SampleTemplatePrograms.cpp
    src/app/LiveGridAuthoringModel.cpp
    src/app/LiveRecordingModel.cpp
    src/app/MissingNodeWorkbenchModel.cpp
    src/app/OnScreenKeyboardModel.cpp
    src/app/ParameterFieldEditors.cpp
    src/app/DspStatusModel.cpp
    src/app/PartialTrackEditorModel.cpp
    src/app/PartialTrackFilters.cpp
    src/app/PartialTrackSpectrumEditing.cpp
    src/app/TalasFiles.cpp
    src/app/MemoryWarnings.cpp
    src/app/PasteHotModel.cpp
    src/app/SoundBrowserAudition.cpp
    src/app/SoundBrowserModel.cpp
    src/app/SoundBrowserSurface.cpp
    src/app/SoundFileWindowModel.cpp
    src/app/VariableValuePrompt.cpp
    src/app/DefaultSounds.cpp
    src/app/SoundClassIcon.cpp
    src/app/TextSearch.cpp
    src/app/PerformanceWorkbenchModel.cpp
    src/app/PresetEventValuesModel.cpp
    src/app/PrototypeBrowserModel.cpp
    src/app/SelectedSoundDocument.cpp
    src/app/SampleMapInteraction.cpp
    src/app/SampleEditorModel.cpp
    src/app/SampleMapWorkbenchModel.cpp
    src/app/SnapshotWorkbenchModel.cpp
    src/app/SoundLibraryModel.cpp
    src/app/SoundLibraryAuditionModel.cpp
    src/app/SoundWorkbenchModel.cpp
    src/app/TimelineInfo.cpp
    src/app/TimelineMarks.cpp
    src/app/TimelineTrackInput.cpp
    src/app/TimelineGrid.cpp
    src/app/VcsDisplays.cpp
    src/app/VcsEditorWindow.cpp
    src/app/VcsLayout.cpp
    src/app/VcsPresets.cpp
    src/app/TimelineMidiImport.cpp
    src/app/TimelineScoreProjection.cpp
    src/app/VirtualControlWidgetModel.cpp
    src/app/WorkbenchDeviceMapping.cpp
    src/app/WorkbenchAccessibilityPolicy.cpp
    src/app/WorkbenchLaunch.cpp
    src/app/WorkbenchOperationCoordinator.cpp
    src/app/WorkbenchSurfaceState.cpp
    src/app/WorkbenchGraphViewportIndex.cpp
    src/app/WorkbenchUiPolicy.cpp
)

target_include_directories(talas_app_support
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(talas_app_support PUBLIC talas_project)

target_compile_features(talas_app_support PUBLIC cxx_std_20)

add_library(talas_performance_runtime_dispatcher
    src/app/PerformanceRuntimeDispatcher.cpp
    src/app/LiveGridWorkbenchModel.cpp
)
target_include_directories(talas_performance_runtime_dispatcher
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(talas_performance_runtime_dispatcher
    PUBLIC
        talas_app_support
        talas_runtime
)
target_compile_features(talas_performance_runtime_dispatcher PUBLIC cxx_std_20)

if(TALAS_BUILD_WORKBENCH)
    if(TALAS_FETCH_JUCE AND NOT TALAS_JUCE_PATH STREQUAL "")
        message(FATAL_ERROR "Choose either TALAS_FETCH_JUCE=ON or TALAS_JUCE_PATH, not both")
    endif()

    if(TALAS_FETCH_JUCE)
        include(FetchContent)
        set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            JUCE
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG ${TALAS_JUCE_GIT_REVISION}
            GIT_SHALLOW FALSE
            EXCLUDE_FROM_ALL
        )
        FetchContent_MakeAvailable(JUCE)
    else()
        if(NOT EXISTS "${TALAS_JUCE_PATH}/CMakeLists.txt")
            message(FATAL_ERROR
                "TALAS_BUILD_WORKBENCH requires a valid TALAS_JUCE_PATH or TALAS_FETCH_JUCE=ON")
        endif()
        add_subdirectory("${TALAS_JUCE_PATH}" "${CMAKE_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)
    endif()

    add_library(talas_workbench_ui STATIC
        src/app/workbench/WorkbenchAnalysisAudition.cpp
        src/app/workbench/WorkbenchAccessibilitySmoke.cpp
        src/app/workbench/WorkbenchScreenshots.cpp
        src/app/workbench/WorkbenchAnalysisPanel.cpp
        src/app/workbench/WorkbenchAnalysisRecording.cpp
        src/app/workbench/WorkbenchLiveTimelineRecording.cpp
        src/app/workbench/WorkbenchAnalysisTools.cpp
        src/app/workbench/WorkbenchAnalysisExamples.cpp
        src/app/workbench/WorkbenchSpectralAnalysisWizard.cpp
        src/app/workbench/WorkbenchSynchronizingSpectra.cpp
        src/app/workbench/WorkbenchSynchronizingSpectraSmoke.cpp
        src/app/workbench/WorkbenchHotValueCompletionSmoke.cpp
        src/app/workbench/WorkbenchDiskSplicerFieldSmoke.cpp
        src/app/workbench/WorkbenchDiskButtonSmoke.cpp
        src/app/workbench/WorkbenchKeyEquivalentsSmoke.cpp
        src/app/workbench/WorkbenchSpectralAnalysisWizardSmoke.cpp
        src/app/workbench/WorkbenchAnalysisToolsSmoke.cpp
        src/app/workbench/WorkbenchCdpProcessTool.cpp
        src/app/workbench/WorkbenchCdpProcessToolSmoke.cpp
        src/app/workbench/WorkbenchAccessibilityHost.h
        src/app/workbench/WorkbenchAnalysisDiagnostics.cpp
        src/app/workbench/WorkbenchAnalogSequencerSmoke.cpp
        src/app/workbench/WorkbenchStereoRoutingSmoke.cpp
        src/app/workbench/WorkbenchShelvingPresenceSmoke.cpp
        src/app/workbench/WorkbenchCheckedBiquadSmoke.cpp
        src/app/workbench/WorkbenchStaticFirSmoke.cpp
        src/app/workbench/WorkbenchVcfSmoke.cpp
        src/app/workbench/WorkbenchHarmonicResonatorSmoke.cpp
        src/app/workbench/WorkbenchDynamicRangeControllerSmoke.cpp
        src/app/workbench/WorkbenchThresholdSmoke.cpp
        src/app/workbench/WorkbenchTimeControlSmoke.cpp
        src/app/workbench/WorkbenchTimeStopperSmoke.cpp
        src/app/workbench/WorkbenchTimeOffsetSmoke.cpp
        src/app/workbench/WorkbenchQuadOscillatorSmoke.cpp
        src/app/workbench/WorkbenchOverlappingMixerSmoke.cpp
        src/app/workbench/WorkbenchOscillatorBankSmoke.cpp
        src/app/workbench/WorkbenchFormantBankSmoke.cpp
        src/app/workbench/WorkbenchFormantBankOscillatorSmoke.cpp
        src/app/workbench/WorkbenchSpectrumModifierSmoke.cpp
        src/app/workbench/WorkbenchPairedStereoOutputSmoke.cpp
        src/app/workbench/WorkbenchSyntheticSpectrumFromSoundsSmoke.cpp
        src/app/workbench/WorkbenchWarpedTimeIndexSmoke.cpp
        src/app/workbench/WorkbenchSpectrumOnDiskSmoke.cpp
        src/app/workbench/WorkbenchSpectrumVoicedUnvoicedSmoke.cpp
        src/app/workbench/WorkbenchSpectrumInRamSmoke.cpp
        src/app/workbench/WorkbenchGaOscillatorsSmoke.cpp
        src/app/workbench/WorkbenchGenericSourceSmoke.cpp
        src/app/workbench/WorkbenchSampleSmoke.cpp
        src/app/workbench/WorkbenchRawMidiOutputSmoke.cpp
        src/app/workbench/WorkbenchMidiControllerOutputSmoke.cpp
        src/app/workbench/WorkbenchAudioMidiSettings.cpp
        src/app/workbench/WorkbenchAssetRecoveryPanel.cpp
        src/app/workbench/WorkbenchCommands.cpp
        src/app/workbench/WorkbenchComponent.cpp
        src/app/workbench/WorkbenchComponentReplacement.cpp
        src/app/workbench/WorkbenchComponentReplacementPanel.cpp
        src/app/workbench/WorkbenchContextHelpPanel.cpp
        src/app/workbench/WorkbenchDiagnosticsPanel.cpp
        src/app/workbench/WorkbenchDiskRecorderSmoke.cpp
        src/app/workbench/WorkbenchDspStatus.cpp
        src/app/workbench/WorkbenchEditOperations.cpp
        src/app/workbench/WorkbenchCompiledSounds.cpp
        src/app/workbench/WorkbenchFiles.cpp
        src/app/workbench/WorkbenchSampleTools.cpp
        src/app/workbench/WorkbenchDiskButton.cpp
        src/app/workbench/WorkbenchSoundHelp.cpp
        src/app/workbench/WorkbenchActiveField.cpp
        src/app/workbench/WorkbenchFocusCoordinator.cpp
        src/app/workbench/WorkbenchFocusIntegration.cpp
        src/app/workbench/WorkbenchGraphCanvas.cpp
        src/app/workbench/WorkbenchGraphArrangement.cpp
        src/app/workbench/WorkbenchGraphArrangementSmoke.cpp
        src/app/workbench/WorkbenchGraphEditingSmoke.cpp
        src/app/workbench/WorkbenchFakeKeyboard.cpp
        src/app/workbench/WorkbenchFakeKeyboardSmoke.cpp
        src/app/workbench/WorkbenchTapeRecorder.cpp
        src/app/workbench/WorkbenchTapeRecorderSmoke.cpp
        src/app/workbench/WorkbenchLiveGridEditor.cpp
        src/app/workbench/WorkbenchLiveGridEditorSmoke.cpp
        src/app/workbench/WorkbenchGeneratorPanel.cpp
        src/app/workbench/WorkbenchGeneratorSmoke.cpp
        src/app/workbench/WorkbenchAuthoringTools.cpp
        src/app/workbench/WorkbenchExpressionsLibrary.cpp
        src/app/workbench/WorkbenchTextFile.cpp
        src/app/workbench/WorkbenchInspectorActionsSmoke.cpp
        src/app/workbench/WorkbenchParameterFieldEditor.cpp
        src/app/workbench/WorkbenchDiskSplicerField.cpp
        src/app/workbench/WorkbenchSampleEditor.cpp
        src/app/workbench/WorkbenchSampleEditorSmoke.cpp
        src/app/workbench/WorkbenchVcsWidgetSmoke.cpp
        src/app/workbench/WorkbenchGlobalMapEditor.cpp
        src/app/workbench/WorkbenchTimelineEditingSmoke.cpp
        src/app/workbench/WorkbenchClassEditor.cpp
        src/app/workbench/WorkbenchClassEditorSmoke.cpp
        src/app/workbench/WorkbenchAlternateTunings.cpp
        src/app/workbench/WorkbenchAlternateTuningsSmoke.cpp
        src/app/workbench/WorkbenchInspectorPanel.cpp
        src/app/workbench/WorkbenchProjectVariablesPanel.cpp
        src/app/workbench/WorkbenchSoundStatePanel.cpp
        src/app/workbench/WorkbenchLayout.cpp
        src/app/workbench/WorkbenchLiveGridRouting.cpp
        src/app/workbench/WorkbenchOscDiagnostics.cpp
        src/app/workbench/WorkbenchOscEgress.cpp
        src/app/workbench/WorkbenchOscIngress.cpp
        src/app/workbench/WorkbenchOscLoopbackProfile.cpp
        src/app/workbench/WorkbenchOscLoopbackProfileCoordinator.cpp
        src/app/workbench/WorkbenchOscRuntime.cpp
        src/app/workbench/WorkbenchOscSmoke.cpp
        src/app/workbench/WorkbenchPartialTrackEditor.cpp
        src/app/workbench/WorkbenchPartialTrackOperations.cpp
        src/app/workbench/WorkbenchPerformanceCanvas.cpp
        src/app/workbench/WorkbenchProjectLookup.cpp
        src/app/workbench/WorkbenchPrototypePanel.cpp
        src/app/workbench/WorkbenchProjectLifecycle.cpp
        src/app/workbench/WorkbenchProcessorPlacementSmoke.cpp
        src/app/workbench/WorkbenchDeviceStopSmoke.cpp
        src/app/workbench/WorkbenchValueGestureHistorySmoke.cpp
        src/app/workbench/WorkbenchProductPerformance.cpp
        src/app/workbench/WorkbenchRuntime.cpp
        src/app/workbench/WorkbenchRuntimeSmoke.cpp
        src/app/workbench/WorkbenchScoreClipArtifacts.cpp
        src/app/workbench/WorkbenchDiskCacheCapture.cpp
        src/app/workbench/WorkbenchSampleMapPanel.cpp
        src/app/workbench/WorkbenchSampleRegionInspector.cpp
        src/app/workbench/WorkbenchSamples.cpp
        src/app/workbench/WorkbenchSmoke.cpp
        src/app/workbench/WorkbenchExpressionLanguageSmoke.cpp
        src/app/workbench/WorkbenchParameterTransformerSmoke.cpp
        src/app/workbench/WorkbenchSoundLibrary.cpp
        src/app/workbench/WorkbenchSoundLibraryAudition.cpp
        src/app/workbench/WorkbenchSoundLibraryAuditionSmoke.cpp
        src/app/workbench/WorkbenchSoundDefinitionsPanel.cpp
        src/app/workbench/WorkbenchSnapshotPanel.cpp
        src/app/workbench/WorkbenchSoundBrowser.cpp
        src/app/workbench/WorkbenchSoundBrowserHost.cpp
        src/app/workbench/WorkbenchSoundBrowserSurface.cpp
        src/app/workbench/WorkbenchSoundFileWindow.cpp
        src/app/workbench/WorkbenchSoundFileHost.cpp
        src/app/workbench/WorkbenchSoundEditors.cpp
        src/app/workbench/WorkbenchCustomPrototypes.cpp
        src/app/workbench/WorkbenchToolEditor.cpp
        src/app/workbench/WorkbenchToolWindow.cpp
        src/app/workbench/WorkbenchToolHost.cpp
        src/app/workbench/WorkbenchSoundIconPainting.cpp
        src/app/workbench/WorkbenchSplitHandle.cpp
        src/app/workbench/WorkbenchTheme.cpp
        src/app/workbench/WorkbenchTimelineInfoStrip.cpp
        src/app/workbench/WorkbenchTimelinePlayback.cpp
        src/app/workbench/WorkbenchWindowMenu.cpp
        src/app/workbench/WorkbenchMenuBar.cpp
        src/app/workbench/WorkbenchFieldReadingMenu.cpp
        src/app/workbench/WorkbenchTimelinePanel.cpp
        src/app/workbench/WorkbenchTimelineSmoke.cpp
        src/app/workbench/WorkbenchTimelineAutomationSmoke.cpp
        src/app/workbench/WorkbenchTimelineInfoSmoke.cpp
        src/app/workbench/WorkbenchTimelineInputSmoke.cpp
        src/app/workbench/WorkbenchTimelinePointsSmoke.cpp
        src/app/workbench/WorkbenchTimelineGridSmoke.cpp
        src/app/workbench/WorkbenchSoundBrowserSmoke.cpp
        src/app/workbench/WorkbenchVcsPresetsSmoke.cpp
        src/app/workbench/WorkbenchSoundDragSmoke.cpp
        src/app/workbench/WorkbenchSoundFileSmoke.cpp
        src/app/workbench/WorkbenchToolSmoke.cpp
        src/app/workbench/WorkbenchUiHelpers.cpp
        src/app/workbench/WorkbenchVcsDisplays.cpp
        src/app/workbench/WorkbenchVcsEditorWindow.cpp
        src/app/workbench/WorkbenchVirtualControls.cpp
        src/app/workbench/WorkbenchWaveformPanel.cpp
    )
    if(APPLE)
        target_sources(talas_workbench_ui PRIVATE
            src/app/workbench/WorkbenchAccessibilityHost_mac.mm)
    else()
        target_sources(talas_workbench_ui PRIVATE
            src/app/workbench/WorkbenchAccessibilityHost.cpp)
    endif()
    target_include_directories(talas_workbench_ui
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(talas_workbench_ui
        PUBLIC
            talas_app_support
            talas_audio_export
            talas_performance_runtime_dispatcher
            juce::juce_audio_utils
            juce::juce_gui_extra
    )
    if(APPLE)
        target_link_libraries(
            talas_workbench_ui
            PRIVATE talas_component_bundle_replacement_controller)
    endif()
    target_compile_features(talas_workbench_ui PUBLIC cxx_std_20)
    target_compile_definitions(talas_workbench_ui
        PUBLIC
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
    )

    juce_add_gui_app(talas_workbench
        COMPANY_NAME "Talas"
        BUNDLE_ID "com.talas.workbench"
        VERSION "${PROJECT_VERSION}"
        PRODUCT_NAME "Talas Workbench"
        MICROPHONE_PERMISSION_ENABLED TRUE
        MICROPHONE_PERMISSION_TEXT "Talas Workbench uses audio input for live Sound processing."
        DOCUMENT_EXTENSIONS pre
    )
    juce_generate_juce_header(talas_workbench)
    target_sources(talas_workbench PRIVATE src/app/WorkbenchMain.cpp)
    target_link_libraries(talas_workbench
        PRIVATE
            talas_workbench_ui
    )
    target_compile_features(talas_workbench PRIVATE cxx_std_20)
    target_compile_definitions(talas_workbench
        PRIVATE
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:talas_workbench,JUCE_PRODUCT_NAME>"
            JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}"
    )
    set(TALAS_SOUND_LIBRARY_BUNDLE_FILES
        examples/sound-library-manifest.json
        examples/c10_midi_mapper_modes.talas.json
        examples/c11_disk_cache_capture.talas.json
        examples/c12_portable_processor_placement.talas.json
        examples/c13_bounded_disk_recorder.talas.json
        examples/c14_bounded_analog_sequencer.talas.json
        examples/c15_stereo_routing_utilities.talas.json
        examples/c16_shelving_presence_filters.talas.json
        examples/c17_checked_biquad_family.talas.json
        examples/c18_bounded_static_fir_family.talas.json
        examples/c19_checked_vcf_tpt.talas.json
        examples/c20_bounded_harmonic_resonator.talas.json
        examples/c21_bounded_dynamic_range_controller.talas.json
        examples/c22_checked_quad_oscillator.talas.json
        examples/c23_bounded_overlapping_mixer.talas.json
        examples/c24_bounded_oscillator_bank.talas.json
        examples/c25_bounded_formant_bank.talas.json
        examples/c26_bounded_formant_bank_oscillator.talas.json
        examples/c27_bounded_spectrum_modifier.talas.json
        examples/c28_paired_stereo_output4.talas.json
        examples/c28_paired_stereo_output8.talas.json
        examples/c29_bounded_synthetic_spectrum_from_sounds.talas.json
        examples/c30_bounded_warped_time_index.talas.json
        examples/c31_bounded_spectrum_on_disk.talas.json
        examples/c32_bounded_ga_oscillators.talas.json
        examples/c33_bounded_generic_source_ram_sample.talas.json
        examples/c35_bounded_multisample.talas.json
        examples/c36_bounded_key_mapped_multisample.talas.json
        examples/c37_bounded_multifile_disk_player.talas.json
        examples/c38_bounded_memory_writer.talas.json
        examples/c39_bounded_raw_midi_output.talas.json
        examples/c40_bounded_midi_controller_output.talas.json
        examples/c41_bounded_midi_note_output.talas.json
        examples/c42_bounded_midi_file_echo.talas.json
        examples/c43_bounded_global_controller_output.talas.json
        examples/c44_dynamic_control_utilities.talas.json
        examples/c45_dynamic_slew_times.talas.json
        examples/c46_dynamic_clock_parameters.talas.json
        examples/c47_dynamic_clock_divider_parameters.talas.json
        examples/c48_dynamic_global_controller_fallback.talas.json
        examples/c49_live_channeller_selectors.talas.json
        examples/c50_live_spectrum_modifier_selectors.talas.json
        examples/c51_live_spectrum_modifier_seed.talas.json
        examples/c52_live_spectrum_modifier_hysteresis.talas.json
        examples/c53_spectrum_modifier_probability_stream.talas.json
        examples/c54_spectrum_modifier_track_probability_profile.talas.json
        examples/c55_spectrum_modifier_remainder_probability_profile.talas.json
        examples/c56_spectrum_on_disk_frame_scan.talas.json
        examples/c57_spectrum_voiced_unvoiced_transition.talas.json
        examples/c58_spectrum_in_ram_normalized_time_index.talas.json
        examples/c59_spectrum_in_ram_default_analysis_metadata.talas.json
        examples/c60_spectrum_on_disk_default_analysis_metadata.talas.json
        examples/c61_sum_of_sines_default_analysis_frequencies.talas.json
        examples/c62_sum_of_sines_controlled_time_frames.talas.json
        examples/c63_sum_of_sines_automatic_gate_loop.talas.json
        examples/c64_synthetic_spectrum_array_prefix.talas.json
        examples/c65_synthetic_spectrum_static_expressions.talas.json
        examples/c66_threshold_hysteresis_gate.talas.json
        examples/c67_time_control_half_speed.talas.json
        examples/c68_time_stopper_sample_hold.talas.json
        examples/c69_time_offset_delay_line.talas.json
        examples/c70_triggered_sample_and_hold_edges.talas.json
        examples/c71_triggered_table_read_gate_reset.talas.json
        examples/c72_tunable_vocoder_bank_reset.talas.json
        examples/c73_two_formant_pair_reset.talas.json
        examples/c74_sound_variable_selector_reset.talas.json
        examples/c75_fixed_vocoder_bank_reset.talas.json
        examples/c76_vocoder_channel_bank_reset.talas.json
        examples/c77_wait_until_buffered_resume.talas.json
        examples/c78_warped_time_index_restart.talas.json
        examples/c79_waveshaper_polynomial_curve.talas.json
        examples/c80_iterated_waveshaper_feedback.talas.json
        examples/c81_multiplying_waveshaper_product.talas.json
        examples/c82_input_output_characteristic_curve.talas.json
        examples/c83_scale_and_offset_affine_control.talas.json
        examples/c84_interpolate_dual_lane_control.talas.json
        examples/c85_running_max_reset_control.talas.json
        examples/c86_running_min_reset_baseline.talas.json
        examples/c87_rms_squared_envelope.talas.json
        examples/c88_amplitude_follower_envelope.talas.json
        examples/c89_peak_detector_envelope.talas.json
        examples/c90_frequency_tracker_history.talas.json
        examples/c91_ar_envelope_state.talas.json
        examples/c92_adsr_typed_gate_state.talas.json
        examples/c93_pulse_train_phase_state.talas.json
        examples/c94_pulse_generator_phase_state.talas.json
        examples/c95_oscillator_phase_state.talas.json
        examples/c96_lfo_phase_transport.talas.json
        examples/c97_ramp_trigger_transport.talas.json
        examples/c98_sample_and_hold_clock_transport.talas.json
        examples/c99_samples_from_disk_single_step_transport.talas.json
        examples/c100_phase_shift_by_90_allpass_transport.talas.json
        examples/c101_single_side_band_rm_quadrature_transport.talas.json
        examples/c102_sos_oscillators_selected_wavetable_transport.talas.json
        examples/c103_low_pass_gate_control_state_transport.talas.json
        examples/c104_reverb_section_fractional_delay_transport.talas.json
        examples/c105_monotonizer_known_pitch_transport.talas.json
        examples/c106_multifile_disk_player_selection_transport.talas.json
        examples/c110_sample_cloud_dynamic_controls.talas.json
        examples/c111_cloud_bank_checked_spectrum.talas.json
        examples/c112_cloud_bank_element_range_cascade_delay.talas.json
        examples/c113_grain_cloud_dynamic_controls.talas.json
        examples/c114_chopper_dynamic_durations.talas.json
        examples/c115_function_generator_trigger_duration.talas.json
        examples/c116_multislope_connected_controls.talas.json
        examples/c117_multisegment_loop_connected_controls.talas.json
        examples/c118_noise_modes_connected_controls.talas.json
        examples/c119_absolute_value_connected_magnitude.talas.json
        examples/c120_arctan_connected_curve.talas.json
        examples/c121_equality_connected_comparison.talas.json
        examples/c122_set_range_connected_affine_mapping.talas.json
        examples/c123_product_connected_finite_multiplication.talas.json
        examples/c124_difference_connected_finite_subtraction.talas.json
        examples/c125_sqrt_magnitude_connected_saturated_norm.talas.json
        examples/c126_vca_connected_finite_gain.talas.json
        examples/c127_constant_finite_event_value_broadcast.talas.json
        examples/c128_add_finite_event_value_sum.talas.json
        examples/c129_mixer_checked_finite_weighted_sum.talas.json
        examples/c130_level_checked_finite_stereo_gain.talas.json
        examples/c131_channel_crosser_checked_finite_matrix.talas.json
        examples/c132_channel_join_checked_finite_dual_lane.talas.json
        examples/c133_channeller_checked_finite_live_selectors.talas.json
        examples/c134_pan_checked_finite_live_controls.talas.json
        examples/c135_crossfade_checked_finite_live_controls.talas.json
        examples/c136_stereo_mix2_checked_live_controls.talas.json
        examples/c137_stereo_mix4_checked_live_controls.talas.json
        examples/c138_audio_output_exact_stereo_terminal.talas.json
        examples/c139_averaging_low_pass_filter_dynamic_window.talas.json
        examples/c140_delay_with_feedback_dynamic_controls.talas.json
        examples/c141_set_duration_bounded_window.talas.json
        examples/c142_warped_time_index_scheduled_morph.talas.json
        examples/c143_amplitude_follower_scheduled_controls.talas.json
        examples/c144_peak_detector_scheduled_controls.talas.json
        examples/c145_rms_squared_scheduled_controls.talas.json
        examples/c146_running_max_scheduled_reset.talas.json
        examples/c147_running_min_scheduled_reset_baseline.talas.json
        examples/c148_triggered_sample_and_hold_scheduled_edges.talas.json
        examples/c149_triggered_table_read_paired_schedules.talas.json
        examples/c150_function_generator_scheduled_controls.talas.json
        examples/c151_multislope_scheduled_controls.talas.json
        examples/c152_multisegment_scheduled_loop_controls.talas.json
        examples/c153_graphical_envelope_scheduled_rate_controls.talas.json
        examples/c154_adsr_scheduled_typed_gate_controls.talas.json
        examples/c155_oscillator_scheduled_phase_controls.talas.json
        examples/c156_chopper_scheduled_cycle_controls.talas.json
        examples/c157_analog_sequencer_scheduled_controls.talas.json
        examples/c158_ar_scheduled_controls.talas.json
        examples/c159_analysis_filter_scheduled_controls.talas.json
        examples/c160_phase_shift_by_90_scheduled_frequency.talas.json
        examples/c161_single_side_band_rm_scheduled_quadrature.talas.json
        examples/c162_sos_oscillators_static_spectrum_sine_bank.talas.json
        examples/c163_low_pass_gate_scheduled_control.talas.json
        examples/c164_morph_filter_scheduled_controls.talas.json
        examples/c165_quad_function_generator_scheduled_controls.talas.json
        examples/c166_probability_chaos_generator_scheduled_controls.talas.json
        examples/c167_stereo_dispersive_resonator_scheduled_controls.talas.json
        examples/c168_plucked_waveguide_scheduled_controls.talas.json
        examples/c169_dynamic_breakpoint_oscillator_scheduled_controls.talas.json
        examples/c170_wave_terrain_oscillator_scheduled_controls.talas.json
        examples/c171_scanned_mass_oscillator_scheduled_controls.talas.json
        examples/c172_diffuse_spectral_stretch_scheduled_controls.talas.json
        examples/c173_dual_morph_oscillator_scheduled_controls.talas.json
        examples/c174_logic_scheduled_fallbacks.talas.json
        examples/c175_annotation_passthrough.talas.json
        examples/c191_filter_scheduled_controls.talas.json
        examples/c200_polyphonic_midi_mapper_input.talas.json
        examples/c201_triggered_global_controller_override.talas.json
        examples/m3_live_spectral_analysis.talas.json
        examples/m3_matrix4_output4.talas.json
        examples/m3_native_fft_round_trip.talas.json
        examples/m3_packed_fft_round_trip.talas.json
        examples/m3_streaming_health.talas.json
        examples/m4_missing_component_preservation.talas.json
        examples/m4_sample_map_instrument.talas.json
        examples/m4_snapshot_performance.talas.json
        examples/m4_state_layers.talas.json
        examples/oscillator.talas.json
        examples/reusable_feedback_help.talas.json
        examples/sample_text_table.talas.json
        examples/DiskCache/c11-disk-cache-loop.wav
        examples/resources/m3-stream-source.wav
        examples/resources/c18-re-kernel.txt
        examples/resources/c31-spectrum-frame.txt
        examples/resources/c32-ga-analysis-a.txt
        examples/resources/c32-ga-analysis-b.txt
        examples/resources/c33-generic-source-sample.txt
        examples/resources/hit.txt
        examples/resources/c42-midi-file-echo.mid
        examples/resources/c56-spectrum-frames.txt
        examples/resources/c57-voiced-unvoiced-frames.txt
        examples/resources/c58-spectrum-in-ram-frames.txt
        examples/resources/c59-spectrum-in-ram-defaults.txt
        examples/resources/c60-spectrum-on-disk-defaults.txt
        examples/resources/c61-sum-of-sines-defaults.txt
        examples/resources/c62-sum-of-sines-controlled-frames.txt
        examples/resources/c62-sum-of-sines-time-index.txt
        examples/resources/c63-sum-of-sines-automatic-frames.txt
        examples/resources/c63-sum-of-sines-gate.txt
        examples/resources/c71-triggered-table-read.txt
        examples/resources/c149-triggered-table-read.txt
        examples/resources/c150-function-generator-envelope.txt
        examples/resources/c170-wave-terrain.txt
        examples/resources/c172-diffuse-spectral-source.txt
        examples/resources/c80-iterated-waveshaper-table.txt
        examples/resources/c81-multiplying-waveshaper-table.txt
        examples/resources/c95-oscillator-cyclic-table.txt
        examples/resources/c99-samples-from-disk-single-step.txt
        examples/resources/c102-sos-oscillators-wave.txt
        examples/README.md
        docs/help/node-example-index.json
        docs/help/builtin-node-reference.md
    )
    if(APPLE)
        set(TALAS_SOUND_LIBRARY_BUNDLE_ROOT
            "$<TARGET_BUNDLE_DIR:talas_workbench>/Contents/Resources/TalasSoundLibrary")
    else()
        set(TALAS_SOUND_LIBRARY_BUNDLE_ROOT
            "$<TARGET_FILE_DIR:talas_workbench>/resources/TalasSoundLibrary")
    endif()
    set(TALAS_SOUND_LIBRARY_BUNDLE_SOURCE_FILES)
    foreach(sound_library_file IN LISTS TALAS_SOUND_LIBRARY_BUNDLE_FILES)
        list(APPEND TALAS_SOUND_LIBRARY_BUNDLE_SOURCE_FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/${sound_library_file}")
    endforeach()
    set(TALAS_SOUND_LIBRARY_BUNDLE_STAMP
        "${CMAKE_CURRENT_BINARY_DIR}/generated/talas_sound_library_bundle_stamp.cpp")
    add_custom_command(
        OUTPUT "${TALAS_SOUND_LIBRARY_BUNDLE_STAMP}"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND ${CMAKE_COMMAND} -E touch
            "${TALAS_SOUND_LIBRARY_BUNDLE_STAMP}"
        DEPENDS
            ${TALAS_SOUND_LIBRARY_BUNDLE_SOURCE_FILES}
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifySoundLibraryBundle.cmake"
        VERBATIM)
    target_sources(talas_workbench PRIVATE "${TALAS_SOUND_LIBRARY_BUNDLE_STAMP}")
    add_custom_command(
        TARGET talas_workbench
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${TALAS_SOUND_LIBRARY_BUNDLE_ROOT}/examples/resources"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${TALAS_SOUND_LIBRARY_BUNDLE_ROOT}/examples/DiskCache"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${TALAS_SOUND_LIBRARY_BUNDLE_ROOT}/docs/help"
        VERBATIM)
    foreach(sound_library_file IN LISTS TALAS_SOUND_LIBRARY_BUNDLE_FILES)
        add_custom_command(
            TARGET talas_workbench
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/${sound_library_file}"
                "${TALAS_SOUND_LIBRARY_BUNDLE_ROOT}/${sound_library_file}"
            VERBATIM)
    endforeach()
    add_custom_command(
        TARGET talas_workbench
        POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DTALAS_SOUND_LIBRARY_ROOT=${TALAS_SOUND_LIBRARY_BUNDLE_ROOT}"
            "-DTALAS_SOUND_LIBRARY_ALLOWED=${TALAS_SOUND_LIBRARY_BUNDLE_FILES}"
            "-DTALAS_SOUND_LIBRARY_REFERENCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifySoundLibraryBundle.cmake"
        VERBATIM)
    add_custom_target(
        talas_sound_library_bundle_verify ALL
        COMMAND ${CMAKE_COMMAND}
            "-DTALAS_SOUND_LIBRARY_ROOT=${TALAS_SOUND_LIBRARY_BUNDLE_ROOT}"
            "-DTALAS_SOUND_LIBRARY_ALLOWED=${TALAS_SOUND_LIBRARY_BUNDLE_FILES}"
            "-DTALAS_SOUND_LIBRARY_REFERENCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifySoundLibraryBundle.cmake"
        DEPENDS talas_workbench
        VERBATIM)
    if(TALAS_BUILD_TESTS)
        target_compile_definitions(talas_workbench PRIVATE
            TALAS_SOUND_LIBRARY_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    endif()

    if(APPLE)
        target_link_libraries(
            talas_workbench
            PRIVATE talas_component_isolation_macos_host)
        target_link_libraries(talas_workbench PRIVATE talas_component_activation_macos_host)
        add_dependencies(talas_workbench talas_component_isolation_helper)
        add_dependencies(talas_workbench talas_component_bundle_probe_helper)
        add_dependencies(talas_workbench talas_component_activation_helper)
        set_property(
            TARGET talas_workbench
            APPEND PROPERTY LINK_DEPENDS
                "$<TARGET_FILE:talas_component_isolation_helper>")
        set_property(
            TARGET talas_workbench
            APPEND PROPERTY LINK_DEPENDS
                "$<TARGET_FILE:talas_component_bundle_probe_helper>")
        set_property(
            TARGET talas_workbench
            APPEND PROPERTY LINK_DEPENDS
                "$<TARGET_FILE:talas_component_activation_helper>")
        add_custom_command(
            TARGET talas_workbench
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_DIR:talas_workbench>/Contents/XPCServices"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "$<TARGET_BUNDLE_DIR:talas_component_isolation_helper>"
                "$<TARGET_BUNDLE_DIR:talas_workbench>/Contents/XPCServices/TalasComponentHost.xpc"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper>"
                "$<TARGET_BUNDLE_DIR:talas_workbench>/Contents/XPCServices/TalasComponentProbe.xpc"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "$<TARGET_BUNDLE_DIR:talas_component_activation_helper>"
                "$<TARGET_BUNDLE_DIR:talas_workbench>/Contents/XPCServices/TalasComponentRuntime.xpc"
            COMMAND /usr/bin/codesign
                --verify
                --strict
                "$<TARGET_BUNDLE_DIR:talas_workbench>/Contents/XPCServices/TalasComponentHost.xpc"
            COMMAND /usr/bin/codesign
                --verify
                --strict
                "$<TARGET_BUNDLE_DIR:talas_workbench>/Contents/XPCServices/TalasComponentProbe.xpc"
            COMMAND /usr/bin/codesign
                --verify
                --strict
                "$<TARGET_BUNDLE_DIR:talas_workbench>/Contents/XPCServices/TalasComponentRuntime.xpc"
            COMMAND /usr/bin/codesign
                --force
                --sign -
                --timestamp=none
                --entitlements
                    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/TalasWorkbench.entitlements"
                "$<TARGET_BUNDLE_DIR:talas_workbench>"
            COMMAND /usr/bin/codesign
                --verify
                --deep
                --strict
                "$<TARGET_BUNDLE_DIR:talas_workbench>"
            VERBATIM)
    endif()

    set(TALAS_WORKBENCH_SMOKE_DIRECTORY
        "${CMAKE_CURRENT_BINARY_DIR}/workbench-smoke")
    file(MAKE_DIRECTORY "${TALAS_WORKBENCH_SMOKE_DIRECTORY}")
    add_test(
        NAME talas_workbench_app_smoke
        COMMAND
            $<TARGET_FILE:talas_workbench>
            --smoke-test
            ${TALAS_WORKBENCH_SMOKE_DIRECTORY}
    )
    set_tests_properties(
        talas_workbench_app_smoke
        PROPERTIES
            RUN_SERIAL TRUE
            FAIL_REGULAR_EXPRESSION "JUCE Assertion failure"
            # Full Debug authoring/library coverage includes migrating and
            # auditing every example; a measured local run takes about 270 s.
            TIMEOUT 600
    )

    if(TALAS_BUILD_TESTS)
        target_compile_definitions(
            talas_workbench_ui PUBLIC
                TALAS_WORKBENCH_OSC_TEST_SUPPORT=1
                TALAS_WORKBENCH_TIMELINE_TEST_SUPPORT=1)
        add_executable(talas_workbench_osc_ingress_tests
            tests/app/WorkbenchOscIngressTests.cpp)
        target_link_libraries(
            talas_workbench_osc_ingress_tests PRIVATE talas_workbench_ui)
        add_test(
            NAME talas_workbench_osc_ingress_tests
            COMMAND talas_workbench_osc_ingress_tests)
        set_tests_properties(
            talas_workbench_osc_ingress_tests PROPERTIES TIMEOUT 30)
        add_executable(talas_workbench_osc_egress_tests
            tests/app/WorkbenchOscEgressTests.cpp)
        target_link_libraries(
            talas_workbench_osc_egress_tests PRIVATE talas_workbench_ui)
        add_test(
            NAME talas_workbench_osc_egress_tests
            COMMAND talas_workbench_osc_egress_tests)
        set_tests_properties(
            talas_workbench_osc_egress_tests PROPERTIES TIMEOUT 30)
        add_executable(talas_workbench_osc_loopback_profile_tests
            tests/app/WorkbenchOscLoopbackProfileTests.cpp)
        target_link_libraries(
            talas_workbench_osc_loopback_profile_tests PRIVATE talas_workbench_ui)
        add_test(
            NAME talas_workbench_osc_loopback_profile_tests
            COMMAND talas_workbench_osc_loopback_profile_tests)
        set_tests_properties(
            talas_workbench_osc_loopback_profile_tests PROPERTIES TIMEOUT 30)
    endif()

    if(TALAS_BUILD_PRODUCT_BENCHMARKS)
        target_compile_definitions(talas_workbench_ui
            PUBLIC TALAS_PRODUCT_PERFORMANCE_PROBE_SUPPORT=1)
        if(NOT CMAKE_GENERATOR STREQUAL "Unix Makefiles")
            message(FATAL_ERROR
                "Product performance probe manifest requires Unix Makefiles")
        endif()
        set(TALAS_PRODUCT_PROBE_BUILD_MANIFEST
            "${CMAKE_CURRENT_BINARY_DIR}/talas-product-probe-build-manifest.json")
        set(TALAS_PRODUCT_PROBE_BUILD_FINGERPRINT_FILE
            "${CMAKE_CURRENT_BINARY_DIR}/talas-product-probe-build-fingerprint.txt")
        string(SHA256 TALAS_PRODUCT_PROBE_CONFIGURATION_FINGERPRINT
            "generator=${CMAKE_GENERATOR}|cmake=${CMAKE_VERSION}|buildType=${CMAKE_BUILD_TYPE}|compiler=${CMAKE_CXX_COMPILER_ID}|compilerVersion=${CMAKE_CXX_COMPILER_VERSION}|juce=${TALAS_PRODUCT_PROBE_JUCE_COMMIT}")
        add_custom_target(talas_product_performance_probe_provenance
            COMMAND ${CMAKE_COMMAND}
                "-DTALAS_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DTALAS_EXPECTED_COMMIT=${TALAS_PRODUCT_PROBE_COMMIT}"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_performance_benchmark_source.cmake
            COMMAND ${CMAKE_COMMAND}
                "-DTALAS_SOURCE_DIR=${TALAS_PRODUCT_PROBE_JUCE_ROOT}"
                "-DTALAS_EXPECTED_COMMIT=${TALAS_PRODUCT_PROBE_JUCE_COMMIT}"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_performance_benchmark_source.cmake
            VERBATIM)
        juce_add_console_app(talas_product_performance_probe
            PRODUCT_NAME "Talas Product Performance Probe")
        add_dependencies(talas_product_performance_probe
            talas_product_performance_probe_provenance)
        target_sources(talas_product_performance_probe
            PRIVATE benchmarks/ProductPerformanceProbe.cpp)
        target_link_libraries(talas_product_performance_probe
            PRIVATE talas_workbench_ui juce::juce_cryptography)
        target_compile_features(talas_product_performance_probe PRIVATE cxx_std_20)
        target_compile_definitions(talas_product_performance_probe
            PRIVATE
                JUCE_WEB_BROWSER=0
                JUCE_USE_CURL=0
                JUCE_MODAL_LOOPS_PERMITTED=1
                TALAS_PRODUCT_PROBE_COMMIT="${TALAS_PRODUCT_PROBE_COMMIT}"
                TALAS_PRODUCT_PROBE_FINGERPRINT_FILE="${TALAS_PRODUCT_PROBE_BUILD_FINGERPRINT_FILE}")
        set_property(TARGET talas_product_performance_probe APPEND PROPERTY
            LINK_DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/capture_compiler_invocation.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/capture_product_probe_compiler.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/write_performance_build_manifest.py")
        add_custom_command(TARGET talas_product_performance_probe POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                "-DTALAS_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DTALAS_EXPECTED_COMMIT=${TALAS_PRODUCT_PROBE_COMMIT}"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_performance_benchmark_source.cmake
            COMMAND ${CMAKE_COMMAND}
                "-DTALAS_SOURCE_DIR=${TALAS_PRODUCT_PROBE_JUCE_ROOT}"
                "-DTALAS_EXPECTED_COMMIT=${TALAS_PRODUCT_PROBE_JUCE_COMMIT}"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_performance_benchmark_source.cmake
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/write_performance_build_manifest.py
                --compile-commands ${CMAKE_CURRENT_BINARY_DIR}/compile_commands.json
                --invocation-log-dir ${TALAS_PRODUCT_PROBE_COMPILER_INVOCATION_LOG_DIR}
                --link-command ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/talas_product_performance_probe.dir/link.txt
                --source-root ${CMAKE_CURRENT_SOURCE_DIR}
                --build-root ${CMAKE_CURRENT_BINARY_DIR}
                --generator "${CMAKE_GENERATOR}"
                --configuration-fingerprint ${TALAS_PRODUCT_PROBE_CONFIGURATION_FINGERPRINT}
                --root-target talas_product_performance_probe
                --executable $<TARGET_FILE:talas_product_performance_probe>
                --manifest ${TALAS_PRODUCT_PROBE_BUILD_MANIFEST}
                --fingerprint ${TALAS_PRODUCT_PROBE_BUILD_FINGERPRINT_FILE}
            BYPRODUCTS
                ${TALAS_PRODUCT_PROBE_BUILD_MANIFEST}
                ${TALAS_PRODUCT_PROBE_BUILD_FINGERPRINT_FILE}
            VERBATIM)
        add_test(
            NAME talas_product_performance_probe_smoke
            COMMAND talas_product_performance_probe
                --warmups=1
                --samples=4
                --stdout)
        set_tests_properties(
            talas_product_performance_probe_smoke
            PROPERTIES LABELS "product-benchmark-smoke" TIMEOUT 60)
    endif()
endif()

include(GNUInstallDirs)

install(
    TARGETS talas_cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(TALAS_BUILD_WORKBENCH)
    install(
        TARGETS talas_workbench
        BUNDLE DESTINATION .
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    if(NOT APPLE)
        foreach(sound_library_file IN LISTS TALAS_SOUND_LIBRARY_BUNDLE_FILES)
            get_filename_component(
                sound_library_directory "${sound_library_file}" DIRECTORY)
            install(
                FILES "${TALAS_SOUND_LIBRARY_BUNDLE_ROOT}/${sound_library_file}"
                DESTINATION
                    "${CMAKE_INSTALL_BINDIR}/resources/TalasSoundLibrary/${sound_library_directory}")
        endforeach()
    endif()
    if(APPLE)
        set(TALAS_SOUND_LIBRARY_INSTALL_RELATIVE_ROOT
            "$<TARGET_BUNDLE_DIR_NAME:talas_workbench>/Contents/Resources/TalasSoundLibrary")
    else()
        set(TALAS_SOUND_LIBRARY_INSTALL_RELATIVE_ROOT
            "${CMAKE_INSTALL_BINDIR}/resources/TalasSoundLibrary")
    endif()
    install(CODE
        "if(IS_ABSOLUTE \"${TALAS_SOUND_LIBRARY_INSTALL_RELATIVE_ROOT}\")
             set(sound_library_install_root
                 \"\$ENV{DESTDIR}${TALAS_SOUND_LIBRARY_INSTALL_RELATIVE_ROOT}\")
         else()
             set(sound_library_install_root
                 \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${TALAS_SOUND_LIBRARY_INSTALL_RELATIVE_ROOT}\")
         endif()
         execute_process(
            COMMAND \"${CMAKE_COMMAND}\"
                \"-DTALAS_SOUND_LIBRARY_ROOT=\${sound_library_install_root}\"
                \"-DTALAS_SOUND_LIBRARY_ALLOWED=${TALAS_SOUND_LIBRARY_BUNDLE_FILES}\"
                \"-DTALAS_SOUND_LIBRARY_REFERENCE_ROOT=${TALAS_SOUND_LIBRARY_BUNDLE_ROOT}\"
                -P \"${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifySoundLibraryBundle.cmake\"
            RESULT_VARIABLE sound_library_verify_result)
         if(NOT sound_library_verify_result EQUAL 0)
             message(FATAL_ERROR \"Installed Sound Library verification failed\")
         endif()")
endif()

install(
    FILES
        README.md
        LICENSE
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
)
install(
    DIRECTORY docs/coverage/
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/coverage
    PATTERN ".DS_Store" EXCLUDE
)

if(TALAS_BUILD_TESTS)
    add_executable(talas_graph_tests
        tests/core/GraphModelTests.cpp
    )
    target_link_libraries(talas_graph_tests PRIVATE talas_core)
    add_test(NAME talas_graph_tests COMMAND talas_graph_tests)

    add_executable(talas_builtin_node_catalog_tests
        tests/core/BuiltinNodeCatalogTests.cpp
    )
    target_link_libraries(talas_builtin_node_catalog_tests PRIVATE talas_core)
    add_test(NAME talas_builtin_node_catalog_tests COMMAND talas_builtin_node_catalog_tests)

    add_executable(talas_node_catalog_json_tests
        tests/core/NodeCatalogJsonTests.cpp
    )
    target_link_libraries(talas_node_catalog_json_tests PRIVATE talas_core)
    target_compile_definitions(talas_node_catalog_json_tests
        PRIVATE TALAS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME talas_node_catalog_json_tests COMMAND talas_node_catalog_json_tests)

    add_executable(talas_expression_tests
        tests/core/ExpressionTests.cpp
    )
    target_link_libraries(talas_expression_tests PRIVATE talas_core)
    add_test(NAME talas_expression_tests COMMAND talas_expression_tests)

    add_executable(talas_val_tests
        tests/core/ValTests.cpp
    )
    target_link_libraries(talas_val_tests PRIVATE talas_core)
    add_test(NAME talas_val_tests COMMAND talas_val_tests)

    add_executable(talas_context_free_generator_tests
        tests/generative/ContextFreeGeneratorTests.cpp
    )
    target_link_libraries(
        talas_context_free_generator_tests PRIVATE talas_generative)
    add_test(
        NAME talas_context_free_generator_tests
        COMMAND talas_context_free_generator_tests)

    add_executable(talas_grammar_tests
        tests/generative/TalasGrammarTests.cpp
    )
    target_link_libraries(talas_grammar_tests PRIVATE talas_generative)
    add_test(
        NAME talas_grammar_tests
        COMMAND talas_grammar_tests)

    add_executable(talas_project_json_property_tests
        tests/property/ProjectJsonPropertyTests.cpp)
    target_link_libraries(talas_project_json_property_tests PRIVATE talas_project)
    add_test(
        NAME talas_project_json_property_tests
        COMMAND talas_project_json_property_tests)

    add_executable(talas_expression_property_tests
        tests/property/ExpressionPropertyTests.cpp)
    target_link_libraries(talas_expression_property_tests PRIVATE talas_core)
    add_test(
        NAME talas_expression_property_tests
        COMMAND talas_expression_property_tests)

    add_executable(talas_context_free_generator_property_tests
        tests/property/ContextFreeGeneratorPropertyTests.cpp)
    target_link_libraries(
        talas_context_free_generator_property_tests PRIVATE talas_generative)
    add_test(
        NAME talas_context_free_generator_property_tests
        COMMAND talas_context_free_generator_property_tests)

    add_executable(talas_graph_property_tests
        tests/property/GraphPropertyTests.cpp)
    target_link_libraries(talas_graph_property_tests PRIVATE talas_core)
    add_test(NAME talas_graph_property_tests COMMAND talas_graph_property_tests)

    add_executable(talas_project_package_property_tests
        tests/property/ProjectPackagePropertyTests.cpp)
    target_link_libraries(talas_project_package_property_tests PRIVATE talas_project)
    add_test(
        NAME talas_project_package_property_tests
        COMMAND talas_project_package_property_tests)

    set_tests_properties(
        talas_project_json_property_tests
        talas_expression_property_tests
        talas_context_free_generator_property_tests
        talas_graph_property_tests
        talas_project_package_property_tests
        PROPERTIES LABELS "property")

    add_executable(talas_spectral_primitives_tests
        tests/dsp/SpectralPrimitivesTests.cpp
    )
    target_link_libraries(talas_spectral_primitives_tests PRIVATE talas_dsp)
    add_test(
        NAME talas_spectral_primitives_tests
        COMMAND talas_spectral_primitives_tests)

    add_executable(talas_native_stft_tests
        tests/dsp/NativeStftTests.cpp
    )
    target_link_libraries(talas_native_stft_tests PRIVATE talas_dsp)
    add_test(NAME talas_native_stft_tests COMMAND talas_native_stft_tests)

    add_executable(talas_phase_vocoder_pitch_tests
        tests/dsp/PhaseVocoderPitchShifterTests.cpp
    )
    target_link_libraries(talas_phase_vocoder_pitch_tests PRIVATE talas_dsp)
    add_test(
        NAME talas_phase_vocoder_pitch_tests
        COMMAND talas_phase_vocoder_pitch_tests)
    # Its wall-clock callback budget is checked only in this serial entry
    # (TALAS_STRICT_TIMING=1, label perf), which local_gate.sh runs after the
    # parallel suite; the entry above prints the timings without checking.
    add_test(NAME talas_phase_vocoder_pitch_tests_strict_timing COMMAND talas_phase_vocoder_pitch_tests)
    set_tests_properties(talas_phase_vocoder_pitch_tests_strict_timing
        PROPERTIES ENVIRONMENT TALAS_STRICT_TIMING=1 LABELS perf RUN_SERIAL TRUE
            TIMEOUT 60)

    add_executable(talas_frequency_scale_granulator_tests tests/dsp/FrequencyScaleGranulatorTests.cpp)
    target_link_libraries(talas_frequency_scale_granulator_tests PRIVATE talas_dsp)
    add_test(NAME talas_frequency_scale_granulator_tests COMMAND talas_frequency_scale_granulator_tests)

    add_executable(talas_time_frequency_scale_dsp_tests tests/dsp/TimeFrequencyScaleTests.cpp)
    target_link_libraries(talas_time_frequency_scale_dsp_tests PRIVATE talas_dsp)
    # The full case list (about 3 minutes in Release) runs as four ctest
    # entries, each taking every fourth case (--shard=K/4), so it no longer
    # sets the suite's critical path.
    foreach(talas_shard RANGE 3)
        add_test(NAME talas_time_frequency_scale_dsp_tests_shard_${talas_shard}
            COMMAND talas_time_frequency_scale_dsp_tests --shard=${talas_shard}/4)
    endforeach()

    add_executable(talas_incremental_lag_subspace_tests tests/dsp/IncrementalLagSubspaceTests.cpp)
    target_link_libraries(talas_incremental_lag_subspace_tests PRIVATE talas_dsp)
    add_test(NAME talas_incremental_lag_subspace_tests COMMAND talas_incremental_lag_subspace_tests)
    set_tests_properties(talas_incremental_lag_subspace_tests PROPERTIES TIMEOUT 120)

    add_executable(talas_incremental_forward_backward_subspace_tests tests/dsp/IncrementalForwardBackwardSubspaceTests.cpp)
    target_link_libraries(talas_incremental_forward_backward_subspace_tests PRIVATE talas_dsp)
    add_test(NAME talas_incremental_forward_backward_subspace_tests COMMAND talas_incremental_forward_backward_subspace_tests)
    set_tests_properties(talas_incremental_forward_backward_subspace_tests PROPERTIES TIMEOUT 120)

    add_executable(talas_incremental_subspace_modes_tests tests/dsp/IncrementalSubspaceModesTests.cpp)
    target_link_libraries(talas_incremental_subspace_modes_tests PRIVATE talas_dsp)
    add_test(NAME talas_incremental_subspace_modes_tests COMMAND talas_incremental_subspace_modes_tests)
    set_tests_properties(talas_incremental_subspace_modes_tests PROPERTIES TIMEOUT 120)

    add_executable(talas_incremental_sinusoid_fit_tests tests/dsp/IncrementalSinusoidFitTests.cpp)
    target_link_libraries(talas_incremental_sinusoid_fit_tests PRIVATE talas_dsp)
    add_test(NAME talas_incremental_sinusoid_fit_tests COMMAND talas_incremental_sinusoid_fit_tests)
    set_tests_properties(talas_incremental_sinusoid_fit_tests PROPERTIES TIMEOUT 120)

    add_executable(talas_incremental_stationary_model_tests tests/dsp/IncrementalStationaryModelTests.cpp)
    target_link_libraries(talas_incremental_stationary_model_tests PRIVATE talas_dsp)
    add_test(NAME talas_incremental_stationary_model_tests COMMAND talas_incremental_stationary_model_tests)
    set_tests_properties(talas_incremental_stationary_model_tests PROPERTIES TIMEOUT 300)

    add_executable(talas_linear_vocoder_bank_tests
        tests/dsp/LinearVocoderBankTests.cpp
    )
    target_link_libraries(talas_linear_vocoder_bank_tests PRIVATE talas_dsp)
    add_test(
        NAME talas_linear_vocoder_bank_tests
        COMMAND talas_linear_vocoder_bank_tests)
    set_tests_properties(
        talas_linear_vocoder_bank_tests
        PROPERTIES TIMEOUT 30)

    add_executable(talas_prepared_granular_scheduler_tests
        tests/dsp/PreparedGranularSchedulerTests.cpp
    )
    target_link_libraries(
        talas_prepared_granular_scheduler_tests PRIVATE talas_dsp)
    add_test(
        NAME talas_prepared_granular_scheduler_tests
        COMMAND talas_prepared_granular_scheduler_tests)
    set_tests_properties(
        talas_prepared_granular_scheduler_tests
        PROPERTIES TIMEOUT 30)

    add_executable(talas_prepared_spectrum_filter_bank_tests
        tests/dsp/PreparedSpectrumFilterBankTests.cpp
    )
    target_link_libraries(
        talas_prepared_spectrum_filter_bank_tests PRIVATE talas_dsp)
    add_test(
        NAME talas_prepared_spectrum_filter_bank_tests
        COMMAND talas_prepared_spectrum_filter_bank_tests)
    set_tests_properties(
        talas_prepared_spectrum_filter_bank_tests
        PROPERTIES TIMEOUT 30)

    add_executable(talas_live_spectral_analysis_tests
        tests/dsp/LiveSpectralAnalysisTests.cpp
    )
    target_link_libraries(talas_live_spectral_analysis_tests PRIVATE talas_dsp)
    add_test(
        NAME talas_live_spectral_analysis_tests
        COMMAND talas_live_spectral_analysis_tests)

    add_executable(talas_packed_real_fft_adapter_tests
        tests/dsp/PackedRealFftAdapterTests.cpp
    )
    target_link_libraries(talas_packed_real_fft_adapter_tests PRIVATE talas_dsp)
    add_test(
        NAME talas_packed_real_fft_adapter_tests
        COMMAND talas_packed_real_fft_adapter_tests)

    add_executable(talas_re_resonator_tests
        tests/dsp/ReResonatorTests.cpp
    )
    target_link_libraries(talas_re_resonator_tests PRIVATE talas_dsp)
    add_test(
        NAME talas_re_resonator_tests
        COMMAND talas_re_resonator_tests)

    add_executable(talas_project_document_tests
        tests/project/ProjectDocumentTests.cpp
    )
    target_link_libraries(talas_project_document_tests PRIVATE talas_project)
    add_test(NAME talas_project_document_tests COMMAND talas_project_document_tests)

    add_executable(talas_project_midi_voice_islands_tests
        tests/project/ProjectMidiVoiceIslandsTests.cpp
    )
    target_link_libraries(talas_project_midi_voice_islands_tests PRIVATE talas_project)
    add_test(NAME talas_project_midi_voice_islands_tests COMMAND talas_project_midi_voice_islands_tests)

    add_executable(talas_project_live_sets_tests
        tests/project/ProjectLiveSetsTests.cpp
    )
    target_link_libraries(talas_project_live_sets_tests PRIVATE talas_project)
    add_test(NAME talas_project_live_sets_tests COMMAND talas_project_live_sets_tests)

    add_executable(talas_project_variables_tests
        tests/project/ProjectVariablesTests.cpp
    )
    target_link_libraries(talas_project_variables_tests PRIVATE talas_project)
    add_test(NAME talas_project_variables_tests COMMAND talas_project_variables_tests)

    add_executable(talas_project_sound_variables_tests
        tests/project/ProjectSoundVariablesTests.cpp
    )
    target_link_libraries(talas_project_sound_variables_tests PRIVATE talas_project)
    add_test(
        NAME talas_project_sound_variables_tests
        COMMAND talas_project_sound_variables_tests)

    add_executable(talas_project_generators_tests
        tests/project/ProjectGeneratorsTests.cpp
    )
    target_link_libraries(talas_project_generators_tests PRIVATE talas_project)
    add_test(
        NAME talas_project_generators_tests
        COMMAND talas_project_generators_tests)

    add_executable(talas_project_generator_outputs_tests
        tests/project/ProjectGeneratorOutputsTests.cpp
    )
    target_link_libraries(
        talas_project_generator_outputs_tests PRIVATE talas_project)
    add_test(
        NAME talas_project_generator_outputs_tests
        COMMAND talas_project_generator_outputs_tests)

    add_executable(talas_project_parameter_transformer_tests
        tests/project/ProjectParameterTransformerTests.cpp
    )
    target_link_libraries(
        talas_project_parameter_transformer_tests PRIVATE talas_project)
    add_test(
        NAME talas_project_parameter_transformer_tests
        COMMAND talas_project_parameter_transformer_tests)
    set_tests_properties(
        talas_project_parameter_transformer_tests PROPERTIES TIMEOUT 30)

    add_executable(talas_smalltalk_subset_tests
        tests/project/SmalltalkSubsetTests.cpp
    )
    target_link_libraries(
        talas_smalltalk_subset_tests PRIVATE talas_project)
    add_test(
        NAME talas_smalltalk_subset_tests
        COMMAND talas_smalltalk_subset_tests)
    set_tests_properties(
        talas_smalltalk_subset_tests PROPERTIES TIMEOUT 30)

    add_executable(talas_tool_tests
        tests/project/ToolTests.cpp
    )
    target_link_libraries(
        talas_tool_tests PRIVATE talas_project)
    add_test(
        NAME talas_tool_tests
        COMMAND talas_tool_tests)
    set_tests_properties(
        talas_tool_tests PROPERTIES TIMEOUT 30)

    add_executable(talas_project_expression_audit_tests
        tests/project/ProjectExpressionAuditTests.cpp
    )
    target_link_libraries(
        talas_project_expression_audit_tests PRIVATE talas_project)
    add_test(
        NAME talas_project_expression_audit_tests
        COMMAND talas_project_expression_audit_tests)

    add_executable(talas_project_migration_compatibility_tests
        tests/project/ProjectMigrationCompatibilityTests.cpp
    )
    target_include_directories(talas_project_migration_compatibility_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/project)
    target_compile_definitions(talas_project_migration_compatibility_tests
        PRIVATE
            "TALAS_PROJECT_MIGRATION_POLICY_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/docs/governance/project-format-migration-policy.json\"")
    target_link_libraries(talas_project_migration_compatibility_tests
        PRIVATE talas_project)
    add_test(
        NAME talas_project_migration_compatibility_tests
        COMMAND talas_project_migration_compatibility_tests)
    set_tests_properties(
        talas_project_migration_compatibility_tests
        PROPERTIES LABELS "migration")

    add_executable(talas_json_value_tests
        tests/project/JsonValueTests.cpp
    )
    target_include_directories(talas_json_value_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/project)
    target_link_libraries(talas_json_value_tests PRIVATE talas_project)
    add_test(NAME talas_json_value_tests COMMAND talas_json_value_tests)

    add_executable(talas_semantic_version_tests
        tests/project/SemanticVersionTests.cpp
    )
    target_include_directories(talas_semantic_version_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(talas_semantic_version_tests PRIVATE talas_project)
    add_test(NAME talas_semantic_version_tests COMMAND talas_semantic_version_tests)

    add_executable(talas_project_missing_node_tests
        tests/project/ProjectMissingNodeTests.cpp
    )
    target_link_libraries(talas_project_missing_node_tests PRIVATE talas_project)
    add_test(
        NAME talas_project_missing_node_tests
        COMMAND talas_project_missing_node_tests)

    add_executable(talas_project_sample_map_tests
        tests/project/ProjectSampleMapTests.cpp
    )
    target_link_libraries(talas_project_sample_map_tests PRIVATE talas_project)
    target_compile_definitions(talas_project_sample_map_tests
        PRIVATE TALAS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(
        NAME talas_project_sample_map_tests
        COMMAND talas_project_sample_map_tests)

    add_executable(talas_project_snapshot_tests
        tests/project/ProjectSnapshotTests.cpp
    )
    target_link_libraries(talas_project_snapshot_tests PRIVATE talas_project)
    add_test(NAME talas_project_snapshot_tests COMMAND talas_project_snapshot_tests)

    add_executable(talas_project_asset_inspection_tests
        tests/project/ProjectAssetInspectionTests.cpp
    )
    target_link_libraries(talas_project_asset_inspection_tests PRIVATE talas_project)
    add_test(
        NAME talas_project_asset_inspection_tests
        COMMAND talas_project_asset_inspection_tests)

    add_executable(talas_partial_track_asset_tests
        tests/project/PartialTrackAssetTests.cpp
    )
    target_link_libraries(talas_partial_track_asset_tests PRIVATE talas_project)
    add_test(
        NAME talas_partial_track_asset_tests
        COMMAND talas_partial_track_asset_tests)

    # .spc spectrum files: read and written as Talas spectra.
    add_executable(talas_sos_spectrum_file_tests
        tests/runtime/SosSpectrumFileTests.cpp
    )
    target_link_libraries(talas_sos_spectrum_file_tests PRIVATE talas_runtime talas_audio)
    add_test(
        NAME talas_sos_spectrum_file_tests
        COMMAND talas_sos_spectrum_file_tests)

    add_executable(talas_project_partial_track_edit_tests
        tests/project/ProjectPartialTrackEditTests.cpp
    )
    target_link_libraries(
        talas_project_partial_track_edit_tests PRIVATE talas_project)
    add_test(
        NAME talas_project_partial_track_edit_tests
        COMMAND talas_project_partial_track_edit_tests)

    add_executable(talas_partial_track_analysis_tests
        tests/project/PartialTrackAnalysisTests.cpp
    )
    target_link_libraries(
        talas_partial_track_analysis_tests PRIVATE talas_audio)
    add_test(
        NAME talas_partial_track_analysis_tests
        COMMAND talas_partial_track_analysis_tests)

    add_executable(talas_partial_track_resynthesis_tests
        tests/audio/PartialTrackResynthesisTests.cpp
    )
    target_link_libraries(
        talas_partial_track_resynthesis_tests PRIVATE talas_audio)
    add_test(
        NAME talas_partial_track_resynthesis_tests
        COMMAND talas_partial_track_resynthesis_tests)

    add_executable(talas_partial_track_derivative_tests
        tests/audio/PartialTrackDerivativeTests.cpp
    )
    target_link_libraries(
        talas_partial_track_derivative_tests PRIVATE talas_audio)
    add_test(
        NAME talas_partial_track_derivative_tests
        COMMAND talas_partial_track_derivative_tests)

    add_executable(talas_project_asset_resolver_tests
        tests/project/ProjectAssetResolverTests.cpp
    )
    target_link_libraries(talas_project_asset_resolver_tests PRIVATE talas_project)
    add_test(
        NAME talas_project_asset_resolver_tests
        COMMAND talas_project_asset_resolver_tests)

    add_executable(talas_project_editor_tests
        tests/project/ProjectEditorTests.cpp
    )
    target_link_libraries(talas_project_editor_tests PRIVATE talas_project)
    add_test(NAME talas_project_editor_tests COMMAND talas_project_editor_tests)

    add_executable(talas_sound_definition_import_tests
        tests/project/SoundDefinitionImportTests.cpp)
    target_link_libraries(talas_sound_definition_import_tests PRIVATE talas_project)
    add_test(NAME talas_sound_definition_import_tests
        COMMAND talas_sound_definition_import_tests)

    add_executable(talas_project_musical_time_tests
        tests/project/ProjectMusicalTimeTests.cpp
    )
    target_link_libraries(talas_project_musical_time_tests PRIVATE talas_project)
    add_test(
        NAME talas_project_musical_time_tests
        COMMAND talas_project_musical_time_tests)

    add_executable(talas_project_score_tests
        tests/project/ProjectScoreTests.cpp
    )
    target_link_libraries(talas_project_score_tests PRIVATE talas_project)
    add_test(NAME talas_project_score_tests COMMAND talas_project_score_tests)

    add_executable(talas_project_disk_cache_tests
        tests/project/ProjectDiskCacheTests.cpp
    )
    target_link_libraries(talas_project_disk_cache_tests PRIVATE talas_project)
    add_test(
        NAME talas_project_disk_cache_tests
        COMMAND talas_project_disk_cache_tests)

    add_executable(talas_controller_recording_tests
        tests/project/ControllerRecordingTests.cpp
    )
    target_link_libraries(talas_controller_recording_tests PRIVATE talas_project)
    add_test(NAME talas_controller_recording_tests
        COMMAND talas_controller_recording_tests)

    add_executable(talas_project_sample_map_editor_tests
        tests/project/ProjectSampleMapEditorTests.cpp
    )
    target_link_libraries(
        talas_project_sample_map_editor_tests
        PRIVATE talas_project)
    add_test(
        NAME talas_project_sample_map_editor_tests
        COMMAND talas_project_sample_map_editor_tests)

    add_executable(talas_project_sample_map_import_editor_tests
        tests/project/ProjectSampleMapImportEditorTests.cpp
    )
    target_link_libraries(
        talas_project_sample_map_import_editor_tests
        PRIVATE talas_project)
    add_test(
        NAME talas_project_sample_map_import_editor_tests
        COMMAND talas_project_sample_map_import_editor_tests)

    add_executable(talas_cdp_nodes_tests
        tests/runtime/CdpNodesTests.cpp
    )
    target_link_libraries(talas_cdp_nodes_tests PRIVATE talas_runtime)
    target_compile_definitions(talas_cdp_nodes_tests PRIVATE
        TALAS_CDP_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/cdp/fixtures")
    add_test(NAME talas_cdp_nodes_tests COMMAND talas_cdp_nodes_tests)

    add_executable(talas_cdp_processors_tests
        tests/cdp/CdpProcessorsTests.cpp
    )
    target_link_libraries(talas_cdp_processors_tests PRIVATE talas_cdp)
    target_compile_definitions(talas_cdp_processors_tests PRIVATE
        TALAS_CDP_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/cdp/fixtures")
    add_test(NAME talas_cdp_processors_tests COMMAND talas_cdp_processors_tests)

    add_executable(talas_cdp_channel_tools_tests
        tests/cdp/CdpChannelToolsTests.cpp
    )
    target_link_libraries(talas_cdp_channel_tools_tests PRIVATE talas_cdp)
    target_compile_definitions(talas_cdp_channel_tools_tests PRIVATE
        TALAS_CDP_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/cdp/fixtures")
    add_test(NAME talas_cdp_channel_tools_tests COMMAND talas_cdp_channel_tools_tests)

    add_executable(talas_cdp_whole_file_tests
        tests/cdp/CdpWholeFileTests.cpp
    )
    target_link_libraries(talas_cdp_whole_file_tests PRIVATE talas_cdp)
    target_compile_definitions(talas_cdp_whole_file_tests PRIVATE
        TALAS_CDP_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/cdp/fixtures")
    add_test(NAME talas_cdp_whole_file_tests COMMAND talas_cdp_whole_file_tests)

    add_executable(talas_cdp_multichannel_tests
        tests/cdp/CdpMultichannelTests.cpp
    )
    target_link_libraries(talas_cdp_multichannel_tests PRIVATE talas_cdp)
    target_compile_definitions(talas_cdp_multichannel_tests PRIVATE
        TALAS_CDP_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/cdp/fixtures")
    add_test(NAME talas_cdp_multichannel_tests COMMAND talas_cdp_multichannel_tests)

    add_executable(talas_cdp_multichannel_nodes_tests
        tests/runtime/CdpMultichannelNodesTests.cpp
    )
    target_link_libraries(talas_cdp_multichannel_nodes_tests PRIVATE talas_runtime)
    target_compile_definitions(talas_cdp_multichannel_nodes_tests PRIVATE
        TALAS_CDP_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/cdp/fixtures")
    add_test(NAME talas_cdp_multichannel_nodes_tests COMMAND talas_cdp_multichannel_nodes_tests)

    add_executable(talas_virtual_control_midi_tests
        tests/project/VirtualControlMidiTests.cpp
    )
    target_link_libraries(talas_virtual_control_midi_tests PRIVATE talas_project)
    add_test(NAME talas_virtual_control_midi_tests COMMAND talas_virtual_control_midi_tests)
    set_tests_properties(
        talas_virtual_control_midi_tests PROPERTIES TIMEOUT 30)

    add_executable(talas_project_global_map_tests
        tests/project/ProjectGlobalMapTests.cpp
    )
    target_link_libraries(talas_project_global_map_tests PRIVATE talas_project)
    add_test(NAME talas_project_global_map_tests COMMAND talas_project_global_map_tests)

    add_executable(talas_alternate_tunings_tests
        tests/project/AlternateTuningsTests.cpp
    )
    target_link_libraries(talas_alternate_tunings_tests PRIVATE talas_project)
    add_test(NAME talas_alternate_tunings_tests COMMAND talas_alternate_tunings_tests)

    add_executable(talas_virtual_control_osc_tests
        tests/project/VirtualControlOscTests.cpp
    )
    target_link_libraries(talas_virtual_control_osc_tests PRIVATE talas_project)
    add_test(NAME talas_virtual_control_osc_tests COMMAND talas_virtual_control_osc_tests)

    add_executable(talas_project_file_tests
        tests/project/ProjectFileTests.cpp
    )
    target_link_libraries(talas_project_file_tests PRIVATE talas_project)
    add_test(NAME talas_project_file_tests COMMAND talas_project_file_tests)

    add_executable(talas_project_edit_boundary_tests
        tests/project/ProjectEditBoundaryTests.cpp
    )
    target_link_libraries(talas_project_edit_boundary_tests PRIVATE talas_project)
    add_test(NAME talas_project_edit_boundary_tests COMMAND talas_project_edit_boundary_tests)

    add_executable(talas_workspace_session_state_tests
        tests/project/WorkspaceSessionStateTests.cpp
    )
    target_link_libraries(talas_workspace_session_state_tests PRIVATE talas_project)
    add_test(NAME talas_workspace_session_state_tests COMMAND talas_workspace_session_state_tests)

    add_executable(talas_state_package_schemas_tests
        tests/project/StatePackageSchemasTests.cpp
    )
    target_link_libraries(talas_state_package_schemas_tests PRIVATE talas_project)
    add_test(NAME talas_state_package_schemas_tests COMMAND talas_state_package_schemas_tests)

    add_executable(talas_project_recovery_file_tests
        tests/project/ProjectRecoveryFileTests.cpp
    )
    target_link_libraries(talas_project_recovery_file_tests PRIVATE talas_project)
    add_test(NAME talas_project_recovery_file_tests COMMAND talas_project_recovery_file_tests)

    add_executable(talas_recent_project_list_tests
        tests/project/RecentProjectListTests.cpp
    )
    target_link_libraries(talas_recent_project_list_tests PRIVATE talas_project)
    add_test(NAME talas_recent_project_list_tests COMMAND talas_recent_project_list_tests)

    add_executable(talas_timeline_interaction_tests
        tests/project/TimelineInteractionTests.cpp
    )
    target_link_libraries(talas_timeline_interaction_tests PRIVATE talas_project)
    add_test(NAME talas_timeline_interaction_tests COMMAND talas_timeline_interaction_tests)

    add_executable(talas_project_autosave_state_tests
        tests/project/ProjectAutosaveStateTests.cpp
    )
    target_link_libraries(talas_project_autosave_state_tests PRIVATE talas_project)
    add_test(NAME talas_project_autosave_state_tests COMMAND talas_project_autosave_state_tests)

    add_executable(talas_project_event_value_discovery_tests
        tests/project/ProjectEventValueDiscoveryTests.cpp
    )
    target_link_libraries(talas_project_event_value_discovery_tests PRIVATE talas_project)
    add_test(NAME talas_project_event_value_discovery_tests COMMAND talas_project_event_value_discovery_tests)

    add_executable(talas_project_local_map_control_discovery_tests
        tests/project/ProjectLocalMapControlDiscoveryTests.cpp)
    target_link_libraries(talas_project_local_map_control_discovery_tests PRIVATE talas_project)
    add_test(NAME talas_project_local_map_control_discovery_tests
        COMMAND talas_project_local_map_control_discovery_tests)

    add_executable(talas_project_device_settings_tests
        tests/project/ProjectDeviceSettingsTests.cpp
    )
    target_link_libraries(talas_project_device_settings_tests PRIVATE talas_project)
    add_test(NAME talas_project_device_settings_tests COMMAND talas_project_device_settings_tests)

    add_executable(talas_compile_plan_tests
        tests/runtime/CompilePlanTests.cpp
    )
    target_link_libraries(talas_compile_plan_tests PRIVATE talas_runtime)
    add_test(NAME talas_compile_plan_tests COMMAND talas_compile_plan_tests)

    add_executable(talas_snapshot_performance_tests
        tests/runtime/SnapshotPerformanceTests.cpp
    )
    target_link_libraries(talas_snapshot_performance_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_snapshot_performance_tests
        COMMAND talas_snapshot_performance_tests)

    add_executable(talas_prepared_graph_summary_tests
        tests/runtime/PreparedGraphSummaryTests.cpp
    )
    target_link_libraries(talas_prepared_graph_summary_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_prepared_graph_summary_tests
        COMMAND talas_prepared_graph_summary_tests
    )

    add_executable(talas_processor_placement_tests
        tests/runtime/ProcessorPlacementTests.cpp
    )
    target_link_libraries(talas_processor_placement_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_processor_placement_tests
        COMMAND talas_processor_placement_tests
    )

    add_executable(talas_latency_compensation_tests
        tests/runtime/LatencyCompensationTests.cpp
    )
    target_link_libraries(talas_latency_compensation_tests PRIVATE talas_runtime)
    add_test(NAME talas_latency_compensation_tests COMMAND talas_latency_compensation_tests)

    add_executable(talas_offline_render_tests
        tests/runtime/OfflineRenderTests.cpp
    )
    target_link_libraries(talas_offline_render_tests PRIVATE talas_runtime)
    add_test(NAME talas_offline_render_tests COMMAND talas_offline_render_tests)

    add_executable(talas_sample_map_playback_tests
        tests/runtime/SampleMapPlaybackTests.cpp
    )
    target_link_libraries(talas_sample_map_playback_tests PRIVATE talas_runtime talas_audio)
    add_test(
        NAME talas_sample_map_playback_tests
        COMMAND talas_sample_map_playback_tests
    )

    add_executable(talas_native_offline_checkpoint_tests
        tests/runtime/NativeOfflineCheckpointTests.cpp)
    target_link_libraries(talas_native_offline_checkpoint_tests PRIVATE talas_runtime)
    add_test(NAME talas_native_offline_checkpoint_tests COMMAND talas_native_offline_checkpoint_tests)

    add_executable(talas_spectral_checkpoint_tests
        tests/runtime/SpectralCheckpointTests.cpp)
    target_link_libraries(talas_spectral_checkpoint_tests PRIVATE talas_runtime)
    add_test(NAME talas_spectral_checkpoint_tests COMMAND talas_spectral_checkpoint_tests)

    add_executable(talas_partial_track_checkpoint_tests
        tests/runtime/PartialTrackCheckpointTests.cpp)
    target_link_libraries(talas_partial_track_checkpoint_tests PRIVATE talas_runtime)
    add_test(NAME talas_partial_track_checkpoint_tests COMMAND talas_partial_track_checkpoint_tests)

    add_executable(talas_time_frequency_scale_checkpoint_tests
        tests/runtime/TimeFrequencyScaleCheckpointTests.cpp)
    target_link_libraries(talas_time_frequency_scale_checkpoint_tests PRIVATE talas_runtime)
    add_test(NAME talas_time_frequency_scale_checkpoint_tests COMMAND talas_time_frequency_scale_checkpoint_tests)

    add_executable(talas_frequency_scale_checkpoint_tests
        tests/runtime/FrequencyScaleCheckpointTests.cpp)
    target_link_libraries(talas_frequency_scale_checkpoint_tests PRIVATE talas_runtime)
    add_test(NAME talas_frequency_scale_checkpoint_tests COMMAND talas_frequency_scale_checkpoint_tests)

    add_executable(talas_prepared_producer_phases_tests
        tests/runtime/PreparedProducerPhasesTests.cpp)
    target_include_directories(talas_prepared_producer_phases_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(talas_prepared_producer_phases_tests PRIVATE talas_runtime)
    add_test(NAME talas_prepared_producer_phases_tests COMMAND talas_prepared_producer_phases_tests)

    add_executable(talas_prepared_terminal_audio_bindings_tests
        tests/runtime/PreparedTerminalAudioBindingsTests.cpp)
    target_include_directories(talas_prepared_terminal_audio_bindings_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(talas_prepared_terminal_audio_bindings_tests PRIVATE talas_runtime)
    add_test(NAME talas_prepared_terminal_audio_bindings_tests COMMAND talas_prepared_terminal_audio_bindings_tests)

    add_executable(talas_prepared_output_bindings_tests
        tests/runtime/PreparedOutputBindingsTests.cpp)
    target_include_directories(talas_prepared_output_bindings_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(talas_prepared_output_bindings_tests PRIVATE talas_runtime)
    add_test(NAME talas_prepared_output_bindings_tests COMMAND talas_prepared_output_bindings_tests)

    add_executable(talas_prepared_running_extremum_program_tests
        tests/runtime/PreparedRunningExtremumProgramTests.cpp)
    target_include_directories(talas_prepared_running_extremum_program_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(talas_prepared_running_extremum_program_tests PRIVATE talas_runtime)
    add_test(NAME talas_prepared_running_extremum_program_tests COMMAND talas_prepared_running_extremum_program_tests)

    add_executable(talas_prepared_scope_authority_tests
        tests/runtime/PreparedScopeAuthorityTests.cpp)
    target_include_directories(talas_prepared_scope_authority_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(talas_prepared_scope_authority_tests PRIVATE talas_runtime)
    add_test(NAME talas_prepared_scope_authority_tests COMMAND talas_prepared_scope_authority_tests)

    add_executable(talas_prepared_unary_control_program_tests
        tests/runtime/PreparedUnaryControlProgramTests.cpp)
    target_include_directories(talas_prepared_unary_control_program_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(talas_prepared_unary_control_program_tests PRIVATE talas_runtime)
    add_test(NAME talas_prepared_unary_control_program_tests COMMAND talas_prepared_unary_control_program_tests)

    add_executable(talas_runtime_conformance_tests
        tests/runtime/RuntimeConformanceTests.cpp
    )
    target_link_libraries(talas_runtime_conformance_tests PRIVATE talas_runtime)
    add_test(NAME talas_runtime_conformance_tests COMMAND talas_runtime_conformance_tests)
    set_tests_properties(
        talas_runtime_conformance_tests
        PROPERTIES LABELS "conformance")

    add_executable(talas_kernel_registry_tests
        tests/runtime/KernelRegistryTests.cpp
    )
    target_link_libraries(talas_kernel_registry_tests PRIVATE talas_runtime)
    add_test(NAME talas_kernel_registry_tests COMMAND talas_kernel_registry_tests)

    add_executable(talas_kernel_abi_c_header_tests
        tests/runtime/KernelAbiCHeaderTests.c
    )
    target_include_directories(talas_kernel_abi_c_header_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(talas_kernel_abi_c_header_tests PRIVATE c_std_11)
    add_test(
        NAME talas_kernel_abi_c_header_tests
        COMMAND talas_kernel_abi_c_header_tests)

    add_executable(talas_kernel_abi_layout_tests
        tests/runtime/KernelAbiLayoutTests.cpp
    )
    target_include_directories(talas_kernel_abi_layout_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(talas_kernel_abi_layout_tests PRIVATE cxx_std_20)
    add_test(
        NAME talas_kernel_abi_layout_tests
        COMMAND talas_kernel_abi_layout_tests)

    add_executable(talas_kernel_abi_host_tests
        tests/runtime/KernelAbiHostTests.cpp
    )
    target_link_libraries(talas_kernel_abi_host_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_kernel_abi_host_tests
        COMMAND talas_kernel_abi_host_tests)

    add_executable(talas_kernel_abi_conformance_tests
        tests/runtime/KernelAbiConformanceTests.cpp
    )
    target_link_libraries(talas_kernel_abi_conformance_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_kernel_abi_conformance_tests
        COMMAND talas_kernel_abi_conformance_tests)
    set_tests_properties(
        talas_kernel_abi_conformance_tests
        PROPERTIES LABELS "conformance")

    add_executable(talas_kernel_abi_probe_fixture
        tests/runtime/KernelAbiProbeFixture.cpp
    )
    target_link_libraries(talas_kernel_abi_probe_fixture PRIVATE talas_runtime)

    add_executable(talas_kernel_abi_probe_tests
        tests/runtime/KernelAbiProbeTests.cpp
    )
    target_link_libraries(talas_kernel_abi_probe_tests PRIVATE talas_runtime)
    add_dependencies(
        talas_kernel_abi_probe_tests
        talas_kernel_abi_probe_fixture)
    add_test(
        NAME talas_kernel_abi_probe_tests
        COMMAND talas_kernel_abi_probe_tests)
    set_tests_properties(talas_kernel_abi_probe_tests PROPERTIES TIMEOUT 15)

    add_executable(talas_component_trust_policy_tests
        tests/runtime/ComponentTrustPolicyTests.cpp
    )
    target_include_directories(talas_component_trust_policy_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(talas_component_trust_policy_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_component_trust_policy_tests
        COMMAND talas_component_trust_policy_tests)

    add_executable(talas_component_bundle_probe_protocol_tests
        tests/runtime/ComponentBundleProbeProtocolTests.cpp
    )
    target_include_directories(talas_component_bundle_probe_protocol_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(talas_component_bundle_probe_protocol_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_component_bundle_probe_protocol_tests
        COMMAND talas_component_bundle_probe_protocol_tests)

    add_executable(talas_component_activation_protocol_tests
        tests/runtime/ComponentActivationProtocolTests.cpp
    )
    target_link_libraries(talas_component_activation_protocol_tests
        PRIVATE talas_runtime)
    add_test(
        NAME talas_component_activation_protocol_tests
        COMMAND talas_component_activation_protocol_tests)

    add_executable(talas_component_activation_authorization_tests
        tests/runtime/ComponentActivationAuthorizationTests.cpp
    )
    target_include_directories(talas_component_activation_authorization_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(talas_component_activation_authorization_tests
        PRIVATE talas_component_activation_authorization)
    add_test(
        NAME talas_component_activation_authorization_tests
        COMMAND talas_component_activation_authorization_tests)

    add_executable(talas_component_activation_audio_data_plane_tests
        tests/runtime/ComponentActivationAudioDataPlaneTests.cpp
    )
    target_link_libraries(talas_component_activation_audio_data_plane_tests
        PRIVATE talas_runtime)
    add_test(
        NAME talas_component_activation_audio_data_plane_tests
        COMMAND talas_component_activation_audio_data_plane_tests)

    add_executable(talas_component_activation_transient_graph_tests
        tests/runtime/ComponentActivationTransientGraphTests.cpp
    )
    target_include_directories(talas_component_activation_transient_graph_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(talas_component_activation_transient_graph_tests
        PRIVATE talas_runtime)
    add_test(
        NAME talas_component_activation_transient_graph_tests
        COMMAND talas_component_activation_transient_graph_tests)

    add_executable(talas_component_probe_evidence_tests
        tests/runtime/ComponentProbeEvidenceTests.cpp
    )
    target_include_directories(talas_component_probe_evidence_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(talas_component_probe_evidence_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_component_probe_evidence_tests
        COMMAND talas_component_probe_evidence_tests)

    add_executable(talas_component_bundle_macho_validation_tests
        tests/runtime/ComponentBundleMachOValidationTests.cpp
    )
    target_include_directories(talas_component_bundle_macho_validation_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(
        talas_component_bundle_macho_validation_tests
        PRIVATE talas_component_probe_validation)
    add_test(
        NAME talas_component_bundle_macho_validation_tests
        COMMAND talas_component_bundle_macho_validation_tests)
    if(APPLE)
        add_library(talas_component_bundle_probe_clean_fixture MODULE
            tests/runtime/fixtures/ComponentBundleProbeCleanFixture.cpp
        )
        target_include_directories(
            talas_component_bundle_probe_clean_fixture
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_compile_features(
            talas_component_bundle_probe_clean_fixture PRIVATE cxx_std_20)
        set_target_properties(
            talas_component_bundle_probe_clean_fixture
            PROPERTIES
                PREFIX ""
                OUTPUT_NAME "TalasComponentProbeCleanFixture"
        )
        add_custom_command(
            TARGET talas_component_bundle_probe_clean_fixture
            POST_BUILD
            COMMAND /usr/bin/codesign --force
                --sign -
                --timestamp=none
                "$<TARGET_FILE:talas_component_bundle_probe_clean_fixture>"
            VERBATIM)
        add_dependencies(
            talas_component_bundle_macho_validation_tests
            talas_component_bundle_probe_clean_fixture)
        target_compile_definitions(
            talas_component_bundle_macho_validation_tests
            PRIVATE
                "TALAS_COMPONENT_PROBE_CLEAN_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_bundle_probe_clean_fixture>\""
        )
        add_library(talas_component_bundle_probe_controlled_fatal_fixture MODULE
            tests/runtime/fixtures/ComponentBundleProbeControlledFatalFixture.cpp
        )
        target_include_directories(
            talas_component_bundle_probe_controlled_fatal_fixture
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_compile_features(
            talas_component_bundle_probe_controlled_fatal_fixture
            PRIVATE cxx_std_20)
        set_target_properties(
            talas_component_bundle_probe_controlled_fatal_fixture
            PROPERTIES
                PREFIX ""
                OUTPUT_NAME "TalasComponentProbeControlledFatalFixture"
        )
        add_custom_command(
            TARGET talas_component_bundle_probe_controlled_fatal_fixture
            POST_BUILD
            COMMAND /usr/bin/codesign --force
                --sign -
                --timestamp=none
                "$<TARGET_FILE:talas_component_bundle_probe_controlled_fatal_fixture>"
            VERBATIM)
        function(talas_add_component_probe_failure_fixture
            target_name output_name failure_mode)
            add_library(${target_name} MODULE
                tests/runtime/fixtures/ComponentBundleProbeFailureFixture.cpp)
            target_include_directories(
                ${target_name}
                PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
            target_compile_features(${target_name} PRIVATE cxx_std_20)
            target_compile_definitions(
                ${target_name}
                PRIVATE TALAS_COMPONENT_PROBE_FAILURE_MODE=${failure_mode})
            set_target_properties(${target_name} PROPERTIES
                PREFIX ""
                OUTPUT_NAME "${output_name}")
            add_custom_command(
                TARGET ${target_name}
                POST_BUILD
                COMMAND /usr/bin/codesign --force
                    --sign -
                    --timestamp=none
                    "$<TARGET_FILE:${target_name}>"
                VERBATIM)
        endfunction()
        talas_add_component_probe_failure_fixture(
            talas_component_bundle_probe_missing_symbol_fixture
            TalasComponentProbeMissingSymbolFixture
            1)
        talas_add_component_probe_failure_fixture(
            talas_component_bundle_probe_metadata_mismatch_fixture
            TalasComponentProbeMetadataMismatchFixture
            2)
        talas_add_component_probe_failure_fixture(
            talas_component_bundle_probe_constructor_crash_fixture
            TalasComponentProbeConstructorCrashFixture
            3)
        talas_add_component_probe_failure_fixture(
            talas_component_bundle_probe_entry_crash_fixture
            TalasComponentProbeEntryCrashFixture
            4)
        talas_add_component_probe_failure_fixture(
            talas_component_bundle_probe_entry_hang_fixture
            TalasComponentProbeEntryHangFixture
            5)
        talas_add_component_probe_failure_fixture(
            talas_component_bundle_probe_unload_crash_fixture
            TalasComponentProbeUnloadCrashFixture
            6)
        talas_add_component_probe_failure_fixture(
            talas_component_bundle_probe_unload_hang_fixture
            TalasComponentProbeUnloadHangFixture
            7)
        if(CMAKE_CXX_FLAGS MATCHES "-fsanitize")
            target_compile_definitions(
                talas_component_bundle_macho_validation_tests
                PRIVATE TALAS_COMPONENT_PROBE_SANITIZED_BUILD=1)
        endif()
    endif()

    add_executable(talas_component_bundle_probe_abi_validation_tests
        tests/runtime/ComponentBundleProbeAbiValidationTests.cpp
    )
    target_include_directories(
        talas_component_bundle_probe_abi_validation_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(
        talas_component_bundle_probe_abi_validation_tests
        PRIVATE talas_component_probe_validation)
    add_test(
        NAME talas_component_bundle_probe_abi_validation_tests
        COMMAND talas_component_bundle_probe_abi_validation_tests)

    add_executable(talas_component_bundle_manifest_tests
        tests/runtime/ComponentBundleManifestTests.cpp
    )
    target_include_directories(talas_component_bundle_manifest_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(talas_component_bundle_manifest_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_component_bundle_manifest_tests
        COMMAND talas_component_bundle_manifest_tests)

    add_executable(talas_component_owner_authorization_tests
        tests/runtime/ComponentOwnerAuthorizationTests.cpp
    )
    target_include_directories(talas_component_owner_authorization_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(talas_component_owner_authorization_tests
        PRIVATE talas_runtime)
    add_test(
        NAME talas_component_owner_authorization_tests
        COMMAND talas_component_owner_authorization_tests)

    add_executable(talas_component_bundle_dependency_resolver_tests
        tests/runtime/ComponentBundleDependencyResolverTests.cpp
    )
    target_include_directories(talas_component_bundle_dependency_resolver_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(talas_component_bundle_dependency_resolver_tests
        PRIVATE talas_runtime)
    add_test(
        NAME talas_component_bundle_dependency_resolver_tests
        COMMAND talas_component_bundle_dependency_resolver_tests)

    add_executable(talas_component_bundle_preservation_compatibility_tests
        tests/runtime/ComponentBundlePreservationCompatibilityTests.cpp
    )
    target_include_directories(
        talas_component_bundle_preservation_compatibility_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(
        talas_component_bundle_preservation_compatibility_tests
        PRIVATE talas_runtime)
    target_compile_definitions(
        talas_component_bundle_preservation_compatibility_tests
        PRIVATE TALAS_COMPONENT_BUNDLE_CONFIRMATION_TEST_SUPPORT=1)
    add_test(
        NAME talas_component_bundle_preservation_compatibility_tests
        COMMAND talas_component_bundle_preservation_compatibility_tests)

    add_executable(talas_component_bundle_inspection_tests
        tests/runtime/ComponentBundleInspectionTests.cpp
    )
    target_include_directories(talas_component_bundle_inspection_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(talas_component_bundle_inspection_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_component_bundle_inspection_tests
        COMMAND talas_component_bundle_inspection_tests)

    if(APPLE)
        add_executable(talas_component_bundle_signature_verifier_tests
            tests/runtime/ComponentBundleSignatureVerifierTests.cpp
        )
        target_include_directories(talas_component_bundle_signature_verifier_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(talas_component_bundle_signature_verifier_tests
            PRIVATE talas_runtime)
        add_test(
            NAME talas_component_bundle_signature_verifier_tests
            COMMAND talas_component_bundle_signature_verifier_tests)
    endif()

    add_executable(talas_component_isolation_protocol_tests
        tests/runtime/ComponentIsolationProtocolTests.cpp
    )
    target_link_libraries(
        talas_component_isolation_protocol_tests
        PRIVATE talas_runtime)
    add_test(
        NAME talas_component_isolation_protocol_tests
        COMMAND talas_component_isolation_protocol_tests)

    add_executable(talas_component_isolation_audio_data_plane_tests
        tests/runtime/ComponentIsolationAudioDataPlaneTests.cpp
    )
    target_link_libraries(
        talas_component_isolation_audio_data_plane_tests
        PRIVATE talas_runtime)
    add_test(
        NAME talas_component_isolation_audio_data_plane_tests
        COMMAND talas_component_isolation_audio_data_plane_tests)

    add_executable(talas_component_isolation_product_audio_worker_tests
        tests/runtime/ComponentIsolationProductAudioWorkerTests.cpp
    )
    target_include_directories(
        talas_component_isolation_product_audio_worker_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(
        talas_component_isolation_product_audio_worker_tests
        PRIVATE talas_runtime)
    add_test(
        NAME talas_component_isolation_product_audio_worker_tests
        COMMAND talas_component_isolation_product_audio_worker_tests)

    add_executable(talas_component_isolation_control_session_tests
        tests/runtime/ComponentIsolationControlSessionTests.cpp
    )
    target_link_libraries(
        talas_component_isolation_control_session_tests
        PRIVATE talas_runtime)
    add_test(
        NAME talas_component_isolation_control_session_tests
        COMMAND talas_component_isolation_control_session_tests)

    add_executable(talas_component_isolation_product_helper_session_tests
        tests/runtime/ComponentIsolationProductHelperSessionTests.cpp
    )
    target_include_directories(
        talas_component_isolation_product_helper_session_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(
        talas_component_isolation_product_helper_session_tests
        PRIVATE talas_runtime)
    add_test(
        NAME talas_component_isolation_product_helper_session_tests
        COMMAND talas_component_isolation_product_helper_session_tests)

    if(APPLE)
        function(talas_add_component_activation_kernel_fixture
            target_name output_name failure_mode)
            set(fixture_source tests/runtime/fixtures/ComponentActivationGainFixture.cpp)
            if(ARGC GREATER 3)
                set(fixture_source "${ARGV3}")
            endif()
            add_library(${target_name} MODULE ${fixture_source})
            target_include_directories(
                ${target_name}
                PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
            target_compile_features(${target_name} PRIVATE cxx_std_20)
            target_compile_definitions(
                ${target_name}
                PRIVATE TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE=${failure_mode})
            # Keep the untrusted bundle image representative when the host-side
            # loader is sanitizer-instrumented. Sanitizer runtime linkage adds
            # LC_RPATH/@rpath dependencies that the product Mach-O gate must
            # reject, while the loader and its test executable remain fully
            # instrumented.
            if(CMAKE_CXX_FLAGS MATCHES "-fsanitize")
                target_compile_options(
                    ${target_name} PRIVATE -fno-sanitize=all)
                target_link_options(
                    ${target_name} PRIVATE -fno-sanitize=all)
            endif()
            set_target_properties(
                ${target_name}
                PROPERTIES PREFIX "" OUTPUT_NAME "${output_name}")
            add_custom_command(
                TARGET ${target_name}
                POST_BUILD
                COMMAND /usr/bin/codesign --force
                    --sign -
                    --timestamp=none
                    "$<TARGET_FILE:${target_name}>"
                VERBATIM)
        endfunction()

        foreach(checkpoint_mode RANGE 0 3)
            talas_add_component_activation_kernel_fixture(
                talas_component_activation_checkpoint_fixture_${checkpoint_mode}
                TalasComponentActivationCheckpointFixture${checkpoint_mode}
                ${checkpoint_mode}
                tests/runtime/fixtures/ComponentActivationCheckpointFixture.cpp)
        endforeach()
        add_executable(talas_component_activation_checkpoint_loader_tests
            tests/runtime/ComponentActivationCheckpointLoaderTests.cpp)
        target_include_directories(talas_component_activation_checkpoint_loader_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(talas_component_activation_checkpoint_loader_tests
            PRIVATE talas_component_activation_loader talas_runtime)
        foreach(checkpoint_mode RANGE 0 3)
            add_dependencies(talas_component_activation_checkpoint_loader_tests
                talas_component_activation_checkpoint_fixture_${checkpoint_mode})
            target_compile_definitions(talas_component_activation_checkpoint_loader_tests PRIVATE
                "TALAS_CHECKPOINT_FIXTURE_${checkpoint_mode}=\"$<TARGET_FILE:talas_component_activation_checkpoint_fixture_${checkpoint_mode}>\"")
        endforeach()
        add_test(NAME talas_component_activation_checkpoint_loader_tests
            COMMAND talas_component_activation_checkpoint_loader_tests)
        set_tests_properties(talas_component_activation_checkpoint_loader_tests PROPERTIES RUN_SERIAL TRUE TIMEOUT 30)

        foreach(resource_checkpoint_mode IN ITEMS 39 52)
            talas_add_component_activation_kernel_fixture(
                talas_component_activation_resource_checkpoint_fixture_${resource_checkpoint_mode}
                TalasComponentActivationResourceCheckpointFixture${resource_checkpoint_mode}
                ${resource_checkpoint_mode})
            target_compile_definitions(talas_component_activation_resource_checkpoint_fixture_${resource_checkpoint_mode}
                PRIVATE TALAS_COMPONENT_ACTIVATION_CHECKPOINT_FIXTURE=1)
        endforeach()
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_resource_checkpoint_restore_failure_fixture
            TalasComponentActivationResourceCheckpointRestoreFailureFixture
            39)
        target_compile_definitions(talas_component_activation_resource_checkpoint_restore_failure_fixture
            PRIVATE TALAS_COMPONENT_ACTIVATION_CHECKPOINT_FIXTURE=2)

        talas_add_component_activation_kernel_fixture(
            talas_component_activation_gain_fixture
            TalasComponentActivationGainFixture
            0)
        talas_add_component_activation_kernel_fixture(
            talas_component_replacement_gain_fixture
            TalasComponentReplacementGainFixture
            0)
        target_compile_definitions(
            talas_component_replacement_gain_fixture
            PRIVATE
                TALAS_COMPONENT_ACTIVATION_FIXTURE_VERSION="2.0.0"
                TALAS_COMPONENT_ACTIVATION_PROBE_COMPATIBLE=1)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_missing_symbol_fixture
            TalasComponentActivationMissingSymbolFixture
            1)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_metadata_mismatch_fixture
            TalasComponentActivationMetadataMismatchFixture
            2)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_entry_fatal_fixture
            TalasComponentActivationEntryFatalFixture
            3)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_entry_throw_fixture
            TalasComponentActivationEntryThrowFixture
            4)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_entry_crash_fixture
            TalasComponentActivationEntryCrashFixture
            5)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_entry_hang_fixture
            TalasComponentActivationEntryHangFixture
            6)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_create_fatal_fixture
            TalasComponentActivationCreateFatalFixture
            7)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_create_throw_fixture
            TalasComponentActivationCreateThrowFixture
            8)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_create_crash_fixture
            TalasComponentActivationCreateCrashFixture
            9)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_create_hang_fixture
            TalasComponentActivationCreateHangFixture
            10)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_allocation_overflow_fixture
            TalasComponentActivationAllocationOverflowFixture
            11)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_prepare_fatal_fixture
            TalasComponentActivationPrepareFatalFixture
            12)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_prepare_throw_fixture
            TalasComponentActivationPrepareThrowFixture
            13)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_prepare_crash_fixture
            TalasComponentActivationPrepareCrashFixture
            14)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_prepare_hang_fixture
            TalasComponentActivationPrepareHangFixture
            15)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_unknown_status_fixture
            TalasComponentActivationUnknownStatusFixture
            16)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_constructor_crash_fixture
            TalasComponentActivationConstructorCrashFixture
            17)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_constructor_hang_fixture
            TalasComponentActivationConstructorHangFixture
            18)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_destroy_throw_fixture
            TalasComponentActivationDestroyThrowFixture
            19)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_destroy_crash_fixture
            TalasComponentActivationDestroyCrashFixture
            20)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_destroy_hang_fixture
            TalasComponentActivationDestroyHangFixture
            21)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_unload_throw_fixture
            TalasComponentActivationUnloadThrowFixture
            22)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_unload_crash_fixture
            TalasComponentActivationUnloadCrashFixture
            23)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_unload_hang_fixture
            TalasComponentActivationUnloadHangFixture
            24)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_reset_fatal_fixture
            TalasComponentActivationResetFatalFixture
            25)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_reset_throw_fixture
            TalasComponentActivationResetThrowFixture
            26)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_reset_crash_fixture
            TalasComponentActivationResetCrashFixture
            27)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_reset_hang_fixture
            TalasComponentActivationResetHangFixture
            28)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_process_fatal_fixture
            TalasComponentActivationProcessFatalFixture
            29)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_process_throw_fixture
            TalasComponentActivationProcessThrowFixture
            30)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_process_crash_fixture
            TalasComponentActivationProcessCrashFixture
            31)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_process_hang_fixture
            TalasComponentActivationProcessHangFixture
            32)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_process_nonfinite_fixture
            TalasComponentActivationProcessNonfiniteFixture
            33)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_process_unknown_fixture
            TalasComponentActivationProcessUnknownFixture
            34)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_resource_fixture
            TalasComponentActivationResourceFixture
            35)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_resource_late_resolver_fixture
            TalasComponentActivationResourceLateResolverFixture
            36)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_binary_resource_fixture
            TalasComponentActivationBinaryResourceFixture
            37)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_binary_resource_late_resolver_fixture
            TalasComponentActivationBinaryResourceLateResolverFixture
            38)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_audio_stream_fixture
            TalasComponentActivationAudioStreamFixture
            39)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_delayed_canary_fixture
            TalasComponentActivationDelayedCanaryFixture
            39)
        target_compile_definitions(
            talas_component_activation_delayed_canary_fixture
            PRIVATE TALAS_COMPONENT_ACTIVATION_DELAY_FIRST_PROCESS=1)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_audio_stream_late_resolver_fixture
            TalasComponentActivationAudioStreamLateResolverFixture
            40)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_audio_stream_repeated_read_fixture
            TalasComponentActivationAudioStreamRepeatedReadFixture
            41)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_audio_stream_reset_read_fixture
            TalasComponentActivationAudioStreamResetReadFixture
            42)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_audio_stream_other_thread_fixture
            TalasComponentActivationAudioStreamOtherThreadFixture
            43)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_audio_stream_double_resolve_fixture
            TalasComponentActivationAudioStreamDoubleResolveFixture
            44)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_audio_stream_missing_resolve_fixture
            TalasComponentActivationAudioStreamMissingResolveFixture
            45)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_audio_stream_process_crash_fixture
            TalasComponentActivationAudioStreamProcessCrashFixture
            46)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_audio_stream_process_hang_fixture
            TalasComponentActivationAudioStreamProcessHangFixture
            47)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_audio_stream_invalid_pointer_fixture
            TalasComponentActivationAudioStreamInvalidPointerFixture
            48)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_audio_stream_one_frame_fixture
            TalasComponentActivationAudioStreamOneFrameFixture
            49)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_audio_stream_seven_frame_fixture
            TalasComponentActivationAudioStreamSevenFrameFixture
            50)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_audio_stream_invalid_count_fixture
            TalasComponentActivationAudioStreamInvalidCountFixture
            51)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_resource_set_fixture
            TalasComponentActivationResourceSetFixture
            52)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_resource_set_repeated_read_fixture
            TalasComponentActivationResourceSetRepeatedReadFixture
            53)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_resource_set_invalid_token_fixture
            TalasComponentActivationResourceSetInvalidTokenFixture
            54)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_resource_set_missing_resolve_fixture
            TalasComponentActivationResourceSetMissingResolveFixture
            55)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_resource_set_table_binary_fixture
            TalasComponentActivationResourceSetTableBinaryFixture
            56)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_resource_set_table_audio_fixture
            TalasComponentActivationResourceSetTableAudioFixture
            57)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_resource_set_binary_two_audio_fixture
            TalasComponentActivationResourceSetBinaryTwoAudioFixture
            58)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_resource_set_maximum_fixture
            TalasComponentActivationResourceSetMaximumFixture
            59)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_resource_set_process_crash_fixture
            TalasComponentActivationResourceSetProcessCrashFixture
            60)
        talas_add_component_activation_kernel_fixture(
            talas_component_activation_resource_set_process_hang_fixture
            TalasComponentActivationResourceSetProcessHangFixture
            61)

        add_executable(talas_component_isolation_xpc_envelope_tests
            tests/runtime/ComponentIsolationXpcEnvelopeTests.cpp
        )
        target_link_libraries(
            talas_component_isolation_xpc_envelope_tests
            PRIVATE talas_component_isolation_macos_xpc)
        add_test(
            NAME talas_component_isolation_xpc_envelope_tests
            COMMAND talas_component_isolation_xpc_envelope_tests)

        add_executable(talas_component_bundle_probe_xpc_envelope_tests
            tests/runtime/ComponentBundleProbeXpcEnvelopeTests.cpp
        )
        target_link_libraries(
            talas_component_bundle_probe_xpc_envelope_tests
            PRIVATE talas_component_probe_macos_xpc)
        add_test(
            NAME talas_component_bundle_probe_xpc_envelope_tests
            COMMAND talas_component_bundle_probe_xpc_envelope_tests)

        add_executable(talas_component_activation_xpc_envelope_tests
            tests/runtime/ComponentActivationXpcEnvelopeTests.cpp
        )
        target_link_libraries(talas_component_activation_xpc_envelope_tests
            PRIVATE talas_component_activation_macos_xpc)
        add_test(
            NAME talas_component_activation_xpc_envelope_tests
            COMMAND talas_component_activation_xpc_envelope_tests)

        add_executable(talas_component_activation_audio_stream_tests
            tests/runtime/ComponentActivationAudioStreamTests.cpp)
        target_include_directories(
            talas_component_activation_audio_stream_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(
            talas_component_activation_audio_stream_tests
            PRIVATE talas_component_activation_audio_stream talas_runtime)
        target_compile_features(
            talas_component_activation_audio_stream_tests PRIVATE cxx_std_20)
        add_test(
            NAME talas_component_activation_audio_stream_tests
            COMMAND talas_component_activation_audio_stream_tests)

        add_executable(talas_component_activation_resource_loader_tests
            tests/runtime/ComponentActivationResourceLoaderTests.cpp)
        target_include_directories(
            talas_component_activation_resource_loader_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(
            talas_component_activation_resource_loader_tests
            PRIVATE
                talas_component_activation_macos_xpc
                talas_component_activation_loader
                talas_component_probe_validation)
        target_compile_features(
            talas_component_activation_resource_loader_tests PRIVATE cxx_std_20)
        add_dependencies(
            talas_component_activation_resource_loader_tests
            talas_component_activation_resource_fixture
            talas_component_activation_resource_late_resolver_fixture
            talas_component_activation_binary_resource_fixture
            talas_component_activation_binary_resource_late_resolver_fixture)
        target_compile_definitions(
            talas_component_activation_resource_loader_tests
            PRIVATE
                "TALAS_COMPONENT_ACTIVATION_RESOURCE_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_resource_fixture>\""
                "TALAS_COMPONENT_ACTIVATION_RESOURCE_LATE_RESOLVER_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_resource_late_resolver_fixture>\""
                "TALAS_COMPONENT_ACTIVATION_BINARY_RESOURCE_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_binary_resource_fixture>\""
                "TALAS_COMPONENT_ACTIVATION_BINARY_RESOURCE_LATE_RESOLVER_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_binary_resource_late_resolver_fixture>\"")
        add_test(
            NAME talas_component_activation_resource_loader_tests
            COMMAND talas_component_activation_resource_loader_tests)

        add_executable(talas_component_activation_audio_loader_tests
            tests/runtime/ComponentActivationAudioLoaderTests.cpp)
        add_dependencies(talas_component_activation_audio_loader_tests
            talas_component_activation_resource_checkpoint_restore_failure_fixture)
        target_compile_definitions(talas_component_activation_audio_loader_tests PRIVATE
            "TALAS_RESOURCE_CHECKPOINT_RESTORE_FAILURE=\"$<TARGET_FILE:talas_component_activation_resource_checkpoint_restore_failure_fixture>\"")
        foreach(resource_checkpoint_mode IN ITEMS 39 52)
            add_dependencies(talas_component_activation_audio_loader_tests
                talas_component_activation_resource_checkpoint_fixture_${resource_checkpoint_mode})
            target_compile_definitions(talas_component_activation_audio_loader_tests PRIVATE
                "TALAS_RESOURCE_CHECKPOINT_FIXTURE_${resource_checkpoint_mode}=\"$<TARGET_FILE:talas_component_activation_resource_checkpoint_fixture_${resource_checkpoint_mode}>\"")
        endforeach()
        target_include_directories(
            talas_component_activation_audio_loader_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(
            talas_component_activation_audio_loader_tests
            PRIVATE
                talas_component_activation_loader
                talas_component_activation_worker
                talas_component_activation_audio_stream
                talas_component_activation_macos_xpc
                talas_runtime)
        target_compile_features(
            talas_component_activation_audio_loader_tests PRIVATE cxx_std_20)
        add_dependencies(
            talas_component_activation_audio_loader_tests
            talas_component_activation_audio_stream_fixture
            talas_component_activation_audio_stream_late_resolver_fixture
            talas_component_activation_audio_stream_repeated_read_fixture
            talas_component_activation_audio_stream_reset_read_fixture
            talas_component_activation_audio_stream_other_thread_fixture
            talas_component_activation_audio_stream_double_resolve_fixture
            talas_component_activation_audio_stream_missing_resolve_fixture
            talas_component_activation_resource_set_fixture
            talas_component_activation_resource_set_repeated_read_fixture
            talas_component_activation_resource_set_invalid_token_fixture
            talas_component_activation_resource_set_missing_resolve_fixture)
        target_compile_definitions(
            talas_component_activation_audio_loader_tests
            PRIVATE
                "TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_audio_stream_fixture>\""
                "TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_LATE_RESOLVER_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_audio_stream_late_resolver_fixture>\""
                "TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_REPEATED_READ_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_audio_stream_repeated_read_fixture>\""
                "TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_RESET_READ_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_audio_stream_reset_read_fixture>\""
                "TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_OTHER_THREAD_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_audio_stream_other_thread_fixture>\""
                "TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_DOUBLE_RESOLVE_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_audio_stream_double_resolve_fixture>\""
                "TALAS_COMPONENT_ACTIVATION_AUDIO_STREAM_MISSING_RESOLVE_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_audio_stream_missing_resolve_fixture>\""
                "TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_resource_set_fixture>\""
                "TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_REPEATED_READ_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_resource_set_repeated_read_fixture>\""
                "TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_INVALID_TOKEN_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_resource_set_invalid_token_fixture>\""
                "TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_MISSING_RESOLVE_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_resource_set_missing_resolve_fixture>\"")
        add_test(
            NAME talas_component_activation_audio_loader_tests
            COMMAND talas_component_activation_audio_loader_tests)

        function(talas_add_component_activation_host_integration_test
            target_name bundle_identifier scenario helper_target helper_bundle)
            add_executable(${target_name} MACOSX_BUNDLE
                tests/runtime/ComponentActivationMacOSHostIntegrationTests.cpp)
            target_include_directories(${target_name}
                PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
            target_link_libraries(${target_name}
                PRIVATE talas_component_activation_macos_host)
            target_compile_features(${target_name} PRIVATE cxx_std_20)
            target_compile_definitions(${target_name}
                PRIVATE TALAS_COMPONENT_ACTIVATION_HOST_SCENARIO=${scenario})
            set_target_properties(${target_name} PROPERTIES
                MACOSX_BUNDLE_GUI_IDENTIFIER "${bundle_identifier}"
                MACOSX_BUNDLE_BUNDLE_NAME
                    "Talas Component Activation Host Test")
            if(helper_target STREQUAL "NO_HELPER")
                add_custom_command(
                    TARGET ${target_name}
                    POST_BUILD
                    COMMAND /usr/bin/codesign --force --sign - --timestamp=none
                        "$<TARGET_BUNDLE_DIR:${target_name}>"
                    VERBATIM)
            else()
                add_dependencies(${target_name} ${helper_target})
                set_property(
                    TARGET ${target_name}
                    APPEND PROPERTY LINK_DEPENDS
                        "$<TARGET_FILE:${helper_target}>")
                add_custom_command(
                    TARGET ${target_name}
                    POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        "$<TARGET_BUNDLE_DIR:${target_name}>/Contents/XPCServices"
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "$<TARGET_BUNDLE_DIR:${helper_target}>"
                        "$<TARGET_BUNDLE_DIR:${target_name}>/Contents/XPCServices/${helper_bundle}.xpc"
                    COMMAND /usr/bin/codesign --verify --strict
                        "$<TARGET_BUNDLE_DIR:${target_name}>/Contents/XPCServices/${helper_bundle}.xpc"
                    COMMAND /usr/bin/codesign --force --sign - --timestamp=none
                        "$<TARGET_BUNDLE_DIR:${target_name}>"
                    VERBATIM)
            endif()
            add_test(NAME ${target_name} COMMAND $<TARGET_FILE:${target_name}>)
            set_tests_properties(${target_name}
                PROPERTIES RUN_SERIAL TRUE TIMEOUT 15)
        endfunction()

        function(talas_add_component_activation_runtime_failure_test
            target_name bundle_identifier fixture_target failure_mode)
            talas_add_component_activation_host_integration_test(
                ${target_name}
                ${bundle_identifier}
                7
                talas_component_activation_helper
                TalasComponentRuntime)
            add_dependencies(${target_name} ${fixture_target})
            target_compile_definitions(
                ${target_name}
                PRIVATE
                    TALAS_COMPONENT_ACTIVATION_FAILURE_MODE=${failure_mode}
                    "TALAS_COMPONENT_ACTIVATION_FIXTURE_PATH=\"$<TARGET_FILE:${fixture_target}>\"")
        endfunction()

        function(talas_add_component_activation_teardown_failure_test
            target_name bundle_identifier fixture_target failure_mode)
            talas_add_component_activation_host_integration_test(
                ${target_name}
                ${bundle_identifier}
                8
                talas_component_activation_helper
                TalasComponentRuntime)
            add_dependencies(${target_name} ${fixture_target})
            target_compile_definitions(
                ${target_name}
                PRIVATE
                    TALAS_COMPONENT_ACTIVATION_FAILURE_MODE=${failure_mode}
                    "TALAS_COMPONENT_ACTIVATION_FIXTURE_PATH=\"$<TARGET_FILE:${fixture_target}>\"")
        endfunction()

        function(talas_add_component_activation_audio_failure_test
            target_name bundle_identifier fixture_target failure_mode)
            talas_add_component_activation_host_integration_test(
                ${target_name}
                ${bundle_identifier}
                9
                talas_component_activation_helper
                TalasComponentRuntime)
            add_dependencies(${target_name} ${fixture_target})
            target_compile_definitions(
                ${target_name}
                PRIVATE
                    TALAS_COMPONENT_ACTIVATION_FAILURE_MODE=${failure_mode}
                    "TALAS_COMPONENT_ACTIVATION_FIXTURE_PATH=\"$<TARGET_FILE:${fixture_target}>\"")
        endfunction()

        function(talas_add_component_activation_profile_test
            target_name bundle_identifier sample_rate block_frames channels)
            talas_add_component_activation_host_integration_test(
                ${target_name}
                ${bundle_identifier}
                1
                talas_component_activation_helper
                TalasComponentRuntime)
            add_dependencies(
                ${target_name}
                talas_component_activation_gain_fixture)
            target_compile_definitions(
                ${target_name}
                PRIVATE
                    TALAS_COMPONENT_ACTIVATION_TEST_SAMPLE_RATE=${sample_rate}
                    TALAS_COMPONENT_ACTIVATION_TEST_BLOCK_FRAMES=${block_frames}
                    TALAS_COMPONENT_ACTIVATION_TEST_CHANNELS=${channels}
                    "TALAS_COMPONENT_ACTIVATION_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_gain_fixture>\"")
        endfunction()

        talas_add_component_activation_host_integration_test(
            talas_component_activation_macos_host_integration_tests
            com.talas.component-activation-host-tests
            1
            talas_component_activation_helper
            TalasComponentRuntime)
        add_dependencies(
            talas_component_activation_macos_host_integration_tests
            talas_component_activation_gain_fixture)
        target_compile_definitions(
            talas_component_activation_macos_host_integration_tests
            PRIVATE
                "TALAS_COMPONENT_ACTIVATION_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_gain_fixture>\"")
        talas_add_component_activation_host_integration_test(
            talas_component_activation_resource_validation_integration_tests
            com.talas.component-activation-resource-validation-tests
            13
            talas_component_activation_helper
            TalasComponentRuntime)
        add_dependencies(
            talas_component_activation_resource_validation_integration_tests
            talas_component_activation_resource_fixture)
        target_compile_definitions(
            talas_component_activation_resource_validation_integration_tests
            PRIVATE
                "TALAS_COMPONENT_ACTIVATION_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_resource_fixture>\"")
        talas_add_component_activation_host_integration_test(
            talas_component_activation_binary_resource_validation_integration_tests
            com.talas.component-activation-binary-resource-validation-tests
            15
            talas_component_activation_helper
            TalasComponentRuntime)
        add_dependencies(
            talas_component_activation_binary_resource_validation_integration_tests
            talas_component_activation_binary_resource_fixture)
        target_compile_definitions(
            talas_component_activation_binary_resource_validation_integration_tests
            PRIVATE
                "TALAS_COMPONENT_ACTIVATION_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_binary_resource_fixture>\"")
        talas_add_component_activation_host_integration_test(
            talas_component_activation_audio_source_validation_integration_tests
            com.talas.component-activation-audio-source-validation-tests
            17
            talas_component_activation_helper
            TalasComponentRuntime)
        add_dependencies(
            talas_component_activation_audio_source_validation_integration_tests
            talas_component_activation_audio_stream_fixture)
        target_compile_definitions(
            talas_component_activation_audio_source_validation_integration_tests
            PRIVATE
                "TALAS_COMPONENT_ACTIVATION_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_audio_stream_fixture>\"")
        talas_add_component_activation_host_integration_test(
            talas_component_activation_resource_set_validation_integration_tests
            com.talas.component-activation-resource-set-validation-tests
            18
            talas_component_activation_helper
            TalasComponentRuntime)
        add_dependencies(
            talas_component_activation_resource_set_validation_integration_tests
            talas_component_activation_gain_fixture)
        target_compile_definitions(
            talas_component_activation_resource_set_validation_integration_tests
            PRIVATE
                "TALAS_COMPONENT_ACTIVATION_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_gain_fixture>\"")
        talas_add_component_activation_host_integration_test(
            talas_component_activation_resource_set_runtime_integration_tests
            com.talas.component-activation-resource-set-runtime-tests
            18
            talas_component_activation_helper
            TalasComponentRuntime)
        add_dependencies(
            talas_component_activation_resource_set_runtime_integration_tests
            talas_component_activation_resource_set_fixture)
        target_compile_definitions(
            talas_component_activation_resource_set_runtime_integration_tests
            PRIVATE
                TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_RUNTIME=1
                "TALAS_COMPONENT_ACTIVATION_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_resource_set_fixture>\"")
        function(talas_add_component_activation_resource_set_runtime_test
            target_name bundle_identifier fixture_target fixture_case)
            talas_add_component_activation_host_integration_test(
                ${target_name}
                ${bundle_identifier}
                18
                talas_component_activation_helper
                TalasComponentRuntime)
            add_dependencies(${target_name} ${fixture_target})
            target_compile_definitions(
                ${target_name}
                PRIVATE
                    TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_RUNTIME=1
                    TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_CASE=${fixture_case}
                    "TALAS_COMPONENT_ACTIVATION_FIXTURE_PATH=\"$<TARGET_FILE:${fixture_target}>\"")
        endfunction()
        talas_add_component_activation_resource_set_runtime_test(
            talas_component_activation_resource_set_table_binary_integration_tests
            com.talas.component-activation-resource-set-table-binary-tests
            talas_component_activation_resource_set_table_binary_fixture
            1)
        talas_add_component_activation_resource_set_runtime_test(
            talas_component_activation_resource_set_table_audio_integration_tests
            com.talas.component-activation-resource-set-table-audio-tests
            talas_component_activation_resource_set_table_audio_fixture
            2)
        talas_add_component_activation_resource_set_runtime_test(
            talas_component_activation_multiple_resource_transient_graph_macos_integration_tests
            com.talas.component-activation-multiple-resource-transient-graph-tests
            talas_component_activation_resource_set_table_audio_fixture
            2)
        target_compile_definitions(
            talas_component_activation_multiple_resource_transient_graph_macos_integration_tests
            PRIVATE
                TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_TRANSIENT_GRAPH=1)
        talas_add_component_activation_resource_set_runtime_test(
            talas_component_activation_multiple_resource_two_audio_transient_graph_macos_integration_tests
            com.talas.component-activation-multiple-resource-two-audio-transient-graph-tests
            talas_component_activation_resource_set_binary_two_audio_fixture
            3)
        target_compile_definitions(
            talas_component_activation_multiple_resource_two_audio_transient_graph_macos_integration_tests
            PRIVATE
                TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_TRANSIENT_GRAPH=1)
        talas_add_component_activation_resource_set_runtime_test(
            talas_component_activation_resource_set_binary_two_audio_integration_tests
            com.talas.component-activation-resource-set-binary-two-audio-tests
            talas_component_activation_resource_set_binary_two_audio_fixture
            3)
        talas_add_component_activation_resource_set_runtime_test(
            talas_component_activation_resource_set_maximum_integration_tests
            com.talas.component-activation-resource-set-maximum-tests
            talas_component_activation_resource_set_maximum_fixture
            4)
        talas_add_component_activation_resource_set_runtime_test(
            talas_component_activation_resource_set_repeated_read_integration_tests
            com.talas.component-activation-resource-set-repeated-read-tests
            talas_component_activation_resource_set_repeated_read_fixture
            0)
        talas_add_component_activation_resource_set_runtime_test(
            talas_component_activation_resource_set_invalid_token_integration_tests
            com.talas.component-activation-resource-set-invalid-token-tests
            talas_component_activation_resource_set_invalid_token_fixture
            0)
        talas_add_component_activation_resource_set_runtime_test(
            talas_component_activation_resource_set_missing_resolve_integration_tests
            com.talas.component-activation-resource-set-missing-resolve-tests
            talas_component_activation_resource_set_missing_resolve_fixture
            0)
        talas_add_component_activation_resource_set_runtime_test(
            talas_component_activation_resource_set_process_crash_integration_tests
            com.talas.component-activation-resource-set-process-crash-tests
            talas_component_activation_resource_set_process_crash_fixture
            0)
        talas_add_component_activation_resource_set_runtime_test(
            talas_component_activation_resource_set_process_hang_integration_tests
            com.talas.component-activation-resource-set-process-hang-tests
            talas_component_activation_resource_set_process_hang_fixture
            0)
        talas_add_component_activation_resource_set_runtime_test(
            talas_component_activation_resource_set_revoke_integration_tests
            com.talas.component-activation-resource-set-revoke-tests
            talas_component_activation_resource_set_fixture
            0)
        target_compile_definitions(
            talas_component_activation_resource_set_repeated_read_integration_tests
            PRIVATE
                TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_EXPECT_REJECTION=1)
        target_compile_definitions(
            talas_component_activation_resource_set_invalid_token_integration_tests
            PRIVATE
                TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_EXPECT_REJECTION=1)
        target_compile_definitions(
            talas_component_activation_resource_set_missing_resolve_integration_tests
            PRIVATE
                TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_EXPECT_REJECTION=1)
        target_compile_definitions(
            talas_component_activation_resource_set_process_crash_integration_tests
            PRIVATE
                TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_RUNTIME_FAILURE_MODE=1)
        target_compile_definitions(
            talas_component_activation_resource_set_process_hang_integration_tests
            PRIVATE
                TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_RUNTIME_FAILURE_MODE=2)
        target_compile_definitions(
            talas_component_activation_resource_set_revoke_integration_tests
            PRIVATE
                TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_REVOKE_BEFORE_READ=1)
        talas_add_component_activation_host_integration_test(
            talas_component_activation_audio_stream_active_peer_disconnect_integration_tests
            com.talas.component-activation-audio-stream-active-peer-disconnect-tests
            17
            talas_component_activation_helper
            TalasComponentRuntime)
        add_dependencies(
            talas_component_activation_audio_stream_active_peer_disconnect_integration_tests
            talas_component_activation_audio_stream_fixture)
        target_compile_definitions(
            talas_component_activation_audio_stream_active_peer_disconnect_integration_tests
            PRIVATE
                TALAS_COMPONENT_ACTIVATION_AUDIO_CANCEL_ACTIVE_PEER=1
                "TALAS_COMPONENT_ACTIVATION_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_audio_stream_fixture>\"")
        foreach(checkpoint_fault IN ITEMS 3 4 5 6 7 8)
            talas_add_component_activation_kernel_fixture(
                talas_component_activation_checkpoint_fault_fixture_${checkpoint_fault}
                TalasComponentCheckpointFaultFixture${checkpoint_fault}
                39)
            target_compile_definitions(talas_component_activation_checkpoint_fault_fixture_${checkpoint_fault}
                PRIVATE TALAS_COMPONENT_ACTIVATION_CHECKPOINT_FIXTURE=${checkpoint_fault})
        endforeach()
        foreach(checkpoint_case IN ITEMS single set legacy 3 4 5 6 7 8 9 retry drain_timeout drain_shutdown drain_revoke whole_single whole_set whole_retry whole_legacy whole_restore_fatal whole_commit_fatal whole_capture_timeout whole_restore_timeout whole_realtime_deadline)
            set(checkpoint_scenario 17)
            set(checkpoint_test_mode 1)
            if(checkpoint_case STREQUAL "whole_single")
                set(checkpoint_fixture talas_component_activation_resource_checkpoint_fixture_39)
                set(checkpoint_test_mode 14)
            elseif(checkpoint_case STREQUAL "whole_set")
                set(checkpoint_scenario 18)
                set(checkpoint_fixture talas_component_activation_resource_checkpoint_fixture_52)
                set(checkpoint_test_mode 15)
            elseif(checkpoint_case STREQUAL "whole_retry")
                set(checkpoint_fixture talas_component_activation_checkpoint_fault_fixture_7)
                set(checkpoint_test_mode 16)
            elseif(checkpoint_case STREQUAL "whole_legacy")
                set(checkpoint_fixture talas_component_activation_audio_stream_fixture)
                set(checkpoint_test_mode 17)
            elseif(checkpoint_case STREQUAL "whole_restore_fatal")
                set(checkpoint_fixture talas_component_activation_resource_checkpoint_restore_failure_fixture)
                set(checkpoint_test_mode 18)
            elseif(checkpoint_case STREQUAL "whole_commit_fatal")
                set(checkpoint_fixture talas_component_activation_checkpoint_fault_fixture_8)
                set(checkpoint_test_mode 19)
            elseif(checkpoint_case STREQUAL "whole_capture_timeout")
                set(checkpoint_fixture talas_component_activation_checkpoint_fault_fixture_3)
                set(checkpoint_test_mode 20)
            elseif(checkpoint_case STREQUAL "whole_restore_timeout")
                set(checkpoint_fixture talas_component_activation_checkpoint_fault_fixture_5)
                set(checkpoint_test_mode 21)
            elseif(checkpoint_case STREQUAL "whole_realtime_deadline")
                set(checkpoint_fixture talas_component_activation_audio_stream_process_hang_fixture)
                set(checkpoint_test_mode 22)
            elseif(checkpoint_case STREQUAL "set")
                set(checkpoint_scenario 18)
                set(checkpoint_fixture talas_component_activation_resource_checkpoint_fixture_52)
            elseif(checkpoint_case STREQUAL "single")
                set(checkpoint_fixture talas_component_activation_resource_checkpoint_fixture_39)
            elseif(checkpoint_case MATCHES "^drain_")
                set(checkpoint_fixture talas_component_activation_resource_checkpoint_fixture_39)
                if(checkpoint_case STREQUAL "drain_timeout")
                    set(checkpoint_test_mode 11)
                elseif(checkpoint_case STREQUAL "drain_shutdown")
                    set(checkpoint_test_mode 12)
                else()
                    set(checkpoint_test_mode 13)
                endif()
            elseif(checkpoint_case STREQUAL "legacy")
                set(checkpoint_fixture talas_component_activation_audio_stream_fixture)
                set(checkpoint_test_mode 2)
            elseif(checkpoint_case STREQUAL "7" OR checkpoint_case STREQUAL "8")
                set(checkpoint_fixture talas_component_activation_resource_checkpoint_fixture_39)
                set(checkpoint_test_mode ${checkpoint_case})
            elseif(checkpoint_case STREQUAL "9")
                set(checkpoint_fixture talas_component_activation_resource_checkpoint_restore_failure_fixture)
                set(checkpoint_test_mode 9)
            elseif(checkpoint_case STREQUAL "retry")
                set(checkpoint_fixture talas_component_activation_checkpoint_fault_fixture_7)
                set(checkpoint_test_mode 10)
            else()
                set(checkpoint_fixture talas_component_activation_checkpoint_fault_fixture_${checkpoint_case})
                set(checkpoint_test_mode ${checkpoint_case})
            endif()
            set(checkpoint_target talas_component_activation_checkpoint_${checkpoint_case}_macos_integration_tests)
            talas_add_component_activation_host_integration_test(
                ${checkpoint_target}
                com.talas.component-checkpoint-${checkpoint_case}-tests
                ${checkpoint_scenario}
                talas_component_activation_helper
                TalasComponentRuntime)
            add_dependencies(${checkpoint_target} ${checkpoint_fixture})
            if(checkpoint_case MATCHES "^whole_")
                target_compile_definitions(${checkpoint_target} PRIVATE
                    TALAS_COMPONENT_ACTIVATION_AUDIO_TRANSIENT_GRAPH=1
                    TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_TRANSIENT_GRAPH=1)
            endif()
            target_compile_definitions(${checkpoint_target} PRIVATE
                TALAS_COMPONENT_ACTIVATION_CHECKPOINT_INTEGRATION=${checkpoint_test_mode}
                TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_RUNTIME=1
                "TALAS_COMPONENT_ACTIVATION_FIXTURE_PATH=\"$<TARGET_FILE:${checkpoint_fixture}>\"")
        endforeach()

        talas_add_component_activation_host_integration_test(
            talas_component_activation_audio_stream_transient_graph_macos_integration_tests
            com.talas.component-activation-audio-stream-transient-graph-tests
            17
            talas_component_activation_helper
            TalasComponentRuntime)
        add_dependencies(
            talas_component_activation_audio_stream_transient_graph_macos_integration_tests
            talas_component_activation_audio_stream_fixture)
        target_compile_definitions(
            talas_component_activation_audio_stream_transient_graph_macos_integration_tests
            PRIVATE
                TALAS_COMPONENT_ACTIVATION_AUDIO_TRANSIENT_GRAPH=1
                "TALAS_COMPONENT_ACTIVATION_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_audio_stream_fixture>\"")
        talas_add_component_activation_host_integration_test(
            talas_component_activation_audio_stream_stereo_44100_integration_tests
            com.talas.component-activation-audio-stream-stereo-44100-tests
            17
            talas_component_activation_helper
            TalasComponentRuntime)
        add_dependencies(
            talas_component_activation_audio_stream_stereo_44100_integration_tests
            talas_component_activation_audio_stream_fixture)
        target_compile_definitions(
            talas_component_activation_audio_stream_stereo_44100_integration_tests
            PRIVATE
                TALAS_COMPONENT_ACTIVATION_TEST_SAMPLE_RATE=44100
                TALAS_COMPONENT_ACTIVATION_TEST_BLOCK_FRAMES=256
                TALAS_COMPONENT_ACTIVATION_TEST_CHANNELS=2
                "TALAS_COMPONENT_ACTIVATION_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_audio_stream_fixture>\"")
        talas_add_component_activation_host_integration_test(
            talas_component_activation_delayed_canary_integration_tests
            com.talas.component-activation-delayed-canary-tests
            17
            talas_component_activation_helper
            TalasComponentRuntime)
        add_dependencies(
            talas_component_activation_delayed_canary_integration_tests
            talas_component_activation_delayed_canary_fixture)
        target_compile_definitions(
            talas_component_activation_delayed_canary_integration_tests
            PRIVATE
                TALAS_COMPONENT_ACTIVATION_TEST_SAMPLE_RATE=44100
                TALAS_COMPONENT_ACTIVATION_TEST_BLOCK_FRAMES=256
                TALAS_COMPONENT_ACTIVATION_TEST_CHANNELS=2
                "TALAS_COMPONENT_ACTIVATION_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_delayed_canary_fixture>\"")
        talas_add_component_activation_host_integration_test(
            talas_component_activation_audio_stream_mono_512_integration_tests
            com.talas.component-activation-audio-stream-mono-512-tests
            17
            talas_component_activation_helper
            TalasComponentRuntime)
        add_dependencies(
            talas_component_activation_audio_stream_mono_512_integration_tests
            talas_component_activation_audio_stream_fixture)
        target_compile_definitions(
            talas_component_activation_audio_stream_mono_512_integration_tests
            PRIVATE
                TALAS_COMPONENT_ACTIVATION_TEST_SAMPLE_RATE=48000
                TALAS_COMPONENT_ACTIVATION_TEST_BLOCK_FRAMES=512
                TALAS_COMPONENT_ACTIVATION_TEST_CHANNELS=1
                "TALAS_COMPONENT_ACTIVATION_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_audio_stream_fixture>\"")
        talas_add_component_activation_host_integration_test(
            talas_component_activation_audio_stream_revoke_integration_tests
            com.talas.component-activation-audio-stream-revoke-tests
            17
            talas_component_activation_helper
            TalasComponentRuntime)
        add_dependencies(
            talas_component_activation_audio_stream_revoke_integration_tests
            talas_component_activation_audio_stream_fixture)
        target_compile_definitions(
            talas_component_activation_audio_stream_revoke_integration_tests
            PRIVATE
                TALAS_COMPONENT_ACTIVATION_AUDIO_REVOKE_BEFORE_READ=1
                "TALAS_COMPONENT_ACTIVATION_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_audio_stream_fixture>\"")
        function(talas_add_component_activation_audio_stream_failure_test
            target_name bundle_identifier fixture_target failure_mode)
            talas_add_component_activation_host_integration_test(
                ${target_name}
                ${bundle_identifier}
                17
                talas_component_activation_helper
                TalasComponentRuntime)
            add_dependencies(${target_name} ${fixture_target})
            target_compile_definitions(
                ${target_name}
                PRIVATE
                    TALAS_COMPONENT_ACTIVATION_AUDIO_RUNTIME_FAILURE_MODE=${failure_mode}
                    "TALAS_COMPONENT_ACTIVATION_FIXTURE_PATH=\"$<TARGET_FILE:${fixture_target}>\"")
        endfunction()
        talas_add_component_activation_audio_stream_failure_test(
            talas_component_activation_audio_stream_crash_integration_tests
            com.talas.component-activation-audio-stream-crash-tests
            talas_component_activation_audio_stream_process_crash_fixture
            1)
        talas_add_component_activation_audio_stream_failure_test(
            talas_component_activation_audio_stream_hang_integration_tests
            com.talas.component-activation-audio-stream-hang-tests
            talas_component_activation_audio_stream_process_hang_fixture
            2)
        talas_add_component_activation_audio_stream_failure_test(
            talas_component_activation_audio_stream_invalid_pointer_integration_tests
            com.talas.component-activation-audio-stream-invalid-pointer-tests
            talas_component_activation_audio_stream_invalid_pointer_fixture
            3)
        talas_add_component_activation_audio_stream_failure_test(
            talas_component_activation_audio_stream_invalid_count_integration_tests
            com.talas.component-activation-audio-stream-invalid-count-tests
            talas_component_activation_audio_stream_invalid_count_fixture
            4)
        talas_add_component_activation_host_integration_test(
            talas_component_activation_audio_stream_health_transient_graph_macos_integration_tests
            com.talas.component-activation-audio-stream-health-graph-tests
            17
            talas_component_activation_helper
            TalasComponentRuntime)
        add_dependencies(
            talas_component_activation_audio_stream_health_transient_graph_macos_integration_tests
            talas_component_activation_audio_stream_invalid_pointer_fixture)
        target_compile_definitions(
            talas_component_activation_audio_stream_health_transient_graph_macos_integration_tests
            PRIVATE
                TALAS_COMPONENT_ACTIVATION_AUDIO_TRANSIENT_GRAPH=1
                TALAS_COMPONENT_ACTIVATION_AUDIO_RUNTIME_FAILURE_MODE=3
                "TALAS_COMPONENT_ACTIVATION_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_audio_stream_invalid_pointer_fixture>\"")
        function(talas_add_component_activation_audio_stream_partition_test
            target_name bundle_identifier fixture_target partition_frames)
            talas_add_component_activation_host_integration_test(
                ${target_name}
                ${bundle_identifier}
                17
                talas_component_activation_helper
                TalasComponentRuntime)
            add_dependencies(${target_name} ${fixture_target})
            target_compile_definitions(
                ${target_name}
                PRIVATE
                    TALAS_COMPONENT_ACTIVATION_AUDIO_PARTITION_FRAMES=${partition_frames}
                    "TALAS_COMPONENT_ACTIVATION_FIXTURE_PATH=\"$<TARGET_FILE:${fixture_target}>\"")
        endfunction()
        talas_add_component_activation_audio_stream_partition_test(
            talas_component_activation_audio_stream_one_frame_integration_tests
            com.talas.component-activation-audio-stream-one-frame-tests
            talas_component_activation_audio_stream_one_frame_fixture
            1)
        talas_add_component_activation_audio_stream_partition_test(
            talas_component_activation_audio_stream_seven_frame_integration_tests
            com.talas.component-activation-audio-stream-seven-frame-tests
            talas_component_activation_audio_stream_seven_frame_fixture
            7)
        talas_add_component_activation_host_integration_test(
            talas_component_activation_binary_resource_transient_graph_macos_integration_tests
            com.talas.component-activation-binary-resource-transient-graph-tests
            16
            talas_component_activation_helper
            TalasComponentRuntime)
        add_dependencies(
            talas_component_activation_binary_resource_transient_graph_macos_integration_tests
            talas_component_activation_binary_resource_fixture)
        target_compile_definitions(
            talas_component_activation_binary_resource_transient_graph_macos_integration_tests
            PRIVATE
                "TALAS_COMPONENT_ACTIVATION_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_binary_resource_fixture>\"")
        talas_add_component_activation_host_integration_test(
            talas_component_activation_resource_transient_graph_macos_integration_tests
            com.talas.component-activation-resource-transient-graph-tests
            14
            talas_component_activation_helper
            TalasComponentRuntime)
        add_dependencies(
            talas_component_activation_resource_transient_graph_macos_integration_tests
            talas_component_activation_resource_fixture)
        target_compile_definitions(
            talas_component_activation_resource_transient_graph_macos_integration_tests
            PRIVATE
                "TALAS_COMPONENT_ACTIVATION_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_resource_fixture>\"")
        talas_add_component_activation_host_integration_test(
            talas_component_activation_transient_graph_macos_integration_tests
            com.talas.component-activation-transient-graph-tests
            10
            talas_component_activation_helper
            TalasComponentRuntime)
        add_dependencies(
            talas_component_activation_transient_graph_macos_integration_tests
            talas_component_activation_gain_fixture)
        target_compile_definitions(
            talas_component_activation_transient_graph_macos_integration_tests
            PRIVATE
                "TALAS_COMPONENT_ACTIVATION_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_gain_fixture>\"")
        talas_add_component_activation_host_integration_test(
            talas_component_activation_termination_failure_integration_tests
            com.talas.component-activation-termination-failure-tests
            11
            talas_component_activation_helper
            TalasComponentRuntime)
        add_dependencies(
            talas_component_activation_termination_failure_integration_tests
            talas_component_activation_gain_fixture)
        target_compile_definitions(
            talas_component_activation_termination_failure_integration_tests
            PRIVATE
                "TALAS_COMPONENT_ACTIVATION_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_gain_fixture>\"")
        talas_add_component_activation_host_integration_test(
            talas_component_activation_identity_mismatch_integration_tests
            com.talas.component-activation-identity-mismatch-tests
            12
            talas_component_activation_helper
            TalasComponentRuntime)
        add_dependencies(
            talas_component_activation_identity_mismatch_integration_tests
            talas_component_activation_gain_fixture)
        target_compile_definitions(
            talas_component_activation_identity_mismatch_integration_tests
            PRIVATE
                "TALAS_COMPONENT_ACTIVATION_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_gain_fixture>\"")
        talas_add_component_activation_profile_test(
            talas_component_activation_profile_44100_256_mono_integration_tests
            com.talas.component-activation-profile-44100-256-mono-tests
            44100 256 1)
        talas_add_component_activation_profile_test(
            talas_component_activation_profile_44100_256_stereo_integration_tests
            com.talas.component-activation-profile-44100-256-stereo-tests
            44100 256 2)
        talas_add_component_activation_profile_test(
            talas_component_activation_profile_44100_512_mono_integration_tests
            com.talas.component-activation-profile-44100-512-mono-tests
            44100 512 1)
        talas_add_component_activation_profile_test(
            talas_component_activation_profile_44100_512_stereo_integration_tests
            com.talas.component-activation-profile-44100-512-stereo-tests
            44100 512 2)
        talas_add_component_activation_profile_test(
            talas_component_activation_profile_48000_256_mono_integration_tests
            com.talas.component-activation-profile-48000-256-mono-tests
            48000 256 1)
        talas_add_component_activation_profile_test(
            talas_component_activation_profile_48000_256_stereo_integration_tests
            com.talas.component-activation-profile-48000-256-stereo-tests
            48000 256 2)
        talas_add_component_activation_profile_test(
            talas_component_activation_profile_48000_512_mono_integration_tests
            com.talas.component-activation-profile-48000-512-mono-tests
            48000 512 1)
        talas_add_component_activation_profile_test(
            talas_component_activation_profile_48000_512_stereo_integration_tests
            com.talas.component-activation-profile-48000-512-stereo-tests
            48000 512 2)
        talas_add_component_activation_host_integration_test(
            talas_component_activation_missing_symbol_integration_tests
            com.talas.component-activation-missing-symbol-tests
            7
            talas_component_activation_helper
            TalasComponentRuntime)
        add_dependencies(
            talas_component_activation_missing_symbol_integration_tests
            talas_component_activation_missing_symbol_fixture)
        target_compile_definitions(
            talas_component_activation_missing_symbol_integration_tests
            PRIVATE
                TALAS_COMPONENT_ACTIVATION_FAILURE_MODE=1
                "TALAS_COMPONENT_ACTIVATION_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_activation_missing_symbol_fixture>\"")
        talas_add_component_activation_runtime_failure_test(
            talas_component_activation_metadata_mismatch_integration_tests
            com.talas.component-activation-metadata-mismatch-tests
            talas_component_activation_metadata_mismatch_fixture
            2)
        talas_add_component_activation_runtime_failure_test(
            talas_component_activation_entry_fatal_integration_tests
            com.talas.component-activation-entry-fatal-tests
            talas_component_activation_entry_fatal_fixture
            3)
        talas_add_component_activation_runtime_failure_test(
            talas_component_activation_entry_throw_integration_tests
            com.talas.component-activation-entry-throw-tests
            talas_component_activation_entry_throw_fixture
            4)
        talas_add_component_activation_runtime_failure_test(
            talas_component_activation_entry_crash_integration_tests
            com.talas.component-activation-entry-crash-tests
            talas_component_activation_entry_crash_fixture
            5)
        talas_add_component_activation_runtime_failure_test(
            talas_component_activation_entry_hang_integration_tests
            com.talas.component-activation-entry-hang-tests
            talas_component_activation_entry_hang_fixture
            6)
        talas_add_component_activation_runtime_failure_test(
            talas_component_activation_create_fatal_integration_tests
            com.talas.component-activation-create-fatal-tests
            talas_component_activation_create_fatal_fixture
            7)
        talas_add_component_activation_runtime_failure_test(
            talas_component_activation_create_throw_integration_tests
            com.talas.component-activation-create-throw-tests
            talas_component_activation_create_throw_fixture
            8)
        talas_add_component_activation_runtime_failure_test(
            talas_component_activation_create_crash_integration_tests
            com.talas.component-activation-create-crash-tests
            talas_component_activation_create_crash_fixture
            9)
        talas_add_component_activation_runtime_failure_test(
            talas_component_activation_create_hang_integration_tests
            com.talas.component-activation-create-hang-tests
            talas_component_activation_create_hang_fixture
            10)
        talas_add_component_activation_runtime_failure_test(
            talas_component_activation_allocation_overflow_integration_tests
            com.talas.component-activation-allocation-overflow-tests
            talas_component_activation_allocation_overflow_fixture
            11)
        talas_add_component_activation_runtime_failure_test(
            talas_component_activation_prepare_fatal_integration_tests
            com.talas.component-activation-prepare-fatal-tests
            talas_component_activation_prepare_fatal_fixture
            12)
        talas_add_component_activation_runtime_failure_test(
            talas_component_activation_prepare_throw_integration_tests
            com.talas.component-activation-prepare-throw-tests
            talas_component_activation_prepare_throw_fixture
            13)
        talas_add_component_activation_runtime_failure_test(
            talas_component_activation_prepare_crash_integration_tests
            com.talas.component-activation-prepare-crash-tests
            talas_component_activation_prepare_crash_fixture
            14)
        talas_add_component_activation_runtime_failure_test(
            talas_component_activation_prepare_hang_integration_tests
            com.talas.component-activation-prepare-hang-tests
            talas_component_activation_prepare_hang_fixture
            15)
        talas_add_component_activation_runtime_failure_test(
            talas_component_activation_unknown_status_integration_tests
            com.talas.component-activation-unknown-status-tests
            talas_component_activation_unknown_status_fixture
            16)
        talas_add_component_activation_runtime_failure_test(
            talas_component_activation_constructor_crash_integration_tests
            com.talas.component-activation-constructor-crash-tests
            talas_component_activation_constructor_crash_fixture
            17)
        talas_add_component_activation_runtime_failure_test(
            talas_component_activation_constructor_hang_integration_tests
            com.talas.component-activation-constructor-hang-tests
            talas_component_activation_constructor_hang_fixture
            18)
        talas_add_component_activation_teardown_failure_test(
            talas_component_activation_destroy_throw_integration_tests
            com.talas.component-activation-destroy-throw-tests
            talas_component_activation_destroy_throw_fixture
            19)
        talas_add_component_activation_teardown_failure_test(
            talas_component_activation_destroy_crash_integration_tests
            com.talas.component-activation-destroy-crash-tests
            talas_component_activation_destroy_crash_fixture
            20)
        talas_add_component_activation_teardown_failure_test(
            talas_component_activation_destroy_hang_integration_tests
            com.talas.component-activation-destroy-hang-tests
            talas_component_activation_destroy_hang_fixture
            21)
        talas_add_component_activation_teardown_failure_test(
            talas_component_activation_unload_throw_integration_tests
            com.talas.component-activation-unload-throw-tests
            talas_component_activation_unload_throw_fixture
            22)
        talas_add_component_activation_teardown_failure_test(
            talas_component_activation_unload_crash_integration_tests
            com.talas.component-activation-unload-crash-tests
            talas_component_activation_unload_crash_fixture
            23)
        talas_add_component_activation_teardown_failure_test(
            talas_component_activation_unload_hang_integration_tests
            com.talas.component-activation-unload-hang-tests
            talas_component_activation_unload_hang_fixture
            24)
        talas_add_component_activation_audio_failure_test(
            talas_component_activation_reset_fatal_integration_tests
            com.talas.component-activation-reset-fatal-tests
            talas_component_activation_reset_fatal_fixture
            25)
        talas_add_component_activation_audio_failure_test(
            talas_component_activation_reset_throw_integration_tests
            com.talas.component-activation-reset-throw-tests
            talas_component_activation_reset_throw_fixture
            26)
        talas_add_component_activation_audio_failure_test(
            talas_component_activation_reset_crash_integration_tests
            com.talas.component-activation-reset-crash-tests
            talas_component_activation_reset_crash_fixture
            27)
        talas_add_component_activation_audio_failure_test(
            talas_component_activation_deadline_integration_tests
            com.talas.component-activation-reset-hang-tests
            talas_component_activation_reset_hang_fixture
            28)
        talas_add_component_activation_audio_failure_test(
            talas_component_activation_process_fatal_integration_tests
            com.talas.component-activation-process-fatal-tests
            talas_component_activation_process_fatal_fixture
            29)
        talas_add_component_activation_audio_failure_test(
            talas_component_activation_process_throw_integration_tests
            com.talas.component-activation-process-throw-tests
            talas_component_activation_process_throw_fixture
            30)
        talas_add_component_activation_audio_failure_test(
            talas_component_activation_process_crash_integration_tests
            com.talas.component-activation-process-crash-tests
            talas_component_activation_process_crash_fixture
            31)
        talas_add_component_activation_audio_failure_test(
            talas_component_activation_process_hang_integration_tests
            com.talas.component-activation-process-hang-tests
            talas_component_activation_process_hang_fixture
            32)
        talas_add_component_activation_audio_failure_test(
            talas_component_activation_quarantine_tests
            com.talas.component-activation-process-nonfinite-tests
            talas_component_activation_process_nonfinite_fixture
            33)
        talas_add_component_activation_audio_failure_test(
            talas_component_activation_process_unknown_integration_tests
            com.talas.component-activation-process-unknown-tests
            talas_component_activation_process_unknown_fixture
            34)
        talas_add_component_activation_host_integration_test(
            talas_component_activation_malformed_integration_tests
            com.talas.component-activation-malformed-tests
            2
            talas_component_activation_helper_malformed
            TalasComponentRuntimeMalformed)
        talas_add_component_activation_host_integration_test(
            talas_component_activation_hang_integration_tests
            com.talas.component-activation-hang-tests
            3
            talas_component_activation_helper_hang
            TalasComponentRuntimeHang)
        talas_add_component_activation_host_integration_test(
            talas_component_activation_session_mismatch_integration_tests
            com.talas.component-activation-mismatch-tests
            4
            talas_component_activation_helper_session_mismatch
            TalasComponentRuntimeSessionMismatch)
        talas_add_component_activation_host_integration_test(
            talas_component_activation_missing_peer_integration_tests
            com.talas.component-activation-missing-tests
            5
            NO_HELPER
            Missing)
        talas_add_component_activation_host_integration_test(
            talas_component_activation_heartbeat_hang_integration_tests
            com.talas.component-activation-heartbeat-hang-tests
            6
            talas_component_activation_helper_heartbeat_hang
            TalasComponentRuntimeHeartbeatHang)

        talas_add_component_activation_fixture_helper(
            talas_component_activation_helper_sandbox
            TalasComponentRuntimeSandbox
            com.talas.workbench.component-runtime.fixture.sandbox
            0)
        target_compile_definitions(
            talas_component_activation_helper_sandbox
            PRIVATE
                TALAS_COMPONENT_ACTIVATION_SANDBOX_PROBE_FIXTURE=1
                "TALAS_COMPONENT_ACTIVATION_SANDBOX_SOURCE_BUNDLE_PATH=\"$<TARGET_FILE:talas_component_bundle_probe_clean_fixture>\"")
        target_sources(
            talas_component_activation_helper_sandbox
            PRIVATE tests/runtime/ComponentActivationSandboxProbe.cpp)
        target_link_libraries(
            talas_component_activation_helper_sandbox
            PRIVATE
                "-framework Security"
                "-framework CoreFoundation")
        add_dependencies(
            talas_component_activation_helper_sandbox
            talas_component_bundle_probe_clean_fixture)

        add_executable(talas_component_activation_sandbox_integration_tests
            MACOSX_BUNDLE
            tests/runtime/ComponentActivationSandboxIntegrationTests.cpp)
        target_include_directories(
            talas_component_activation_sandbox_integration_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(
            talas_component_activation_sandbox_integration_tests
            PRIVATE
                talas_component_activation_macos_xpc
                "-framework Security"
                "-framework CoreFoundation")
        target_compile_features(
            talas_component_activation_sandbox_integration_tests
            PRIVATE cxx_std_20)
        target_compile_options(
            talas_component_activation_sandbox_integration_tests
            PRIVATE -fblocks)
        set_target_properties(
            talas_component_activation_sandbox_integration_tests
            PROPERTIES
                MACOSX_BUNDLE_GUI_IDENTIFIER
                    "com.talas.component-activation-sandbox-tests"
                MACOSX_BUNDLE_BUNDLE_NAME
                    "Talas Component Activation Sandbox Tests")
        add_dependencies(
            talas_component_activation_sandbox_integration_tests
            talas_component_activation_helper_sandbox
            talas_component_bundle_probe_helper
            talas_component_isolation_helper)
        set_property(
            TARGET talas_component_activation_sandbox_integration_tests
            APPEND PROPERTY LINK_DEPENDS
                "$<TARGET_FILE:talas_component_activation_helper_sandbox>"
                "$<TARGET_FILE:talas_component_bundle_probe_helper>"
                "$<TARGET_FILE:talas_component_isolation_helper>")
        add_custom_command(
            TARGET talas_component_activation_sandbox_integration_tests
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_DIR:talas_component_activation_sandbox_integration_tests>/Contents/XPCServices"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "$<TARGET_BUNDLE_DIR:talas_component_activation_helper_sandbox>"
                "$<TARGET_BUNDLE_DIR:talas_component_activation_sandbox_integration_tests>/Contents/XPCServices/TalasComponentRuntimeSandbox.xpc"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper>"
                "$<TARGET_BUNDLE_DIR:talas_component_activation_sandbox_integration_tests>/Contents/XPCServices/TalasComponentProbe.xpc"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "$<TARGET_BUNDLE_DIR:talas_component_isolation_helper>"
                "$<TARGET_BUNDLE_DIR:talas_component_activation_sandbox_integration_tests>/Contents/XPCServices/TalasComponentHost.xpc"
            COMMAND /usr/bin/codesign --verify --strict
                "$<TARGET_BUNDLE_DIR:talas_component_activation_sandbox_integration_tests>/Contents/XPCServices/TalasComponentRuntimeSandbox.xpc"
            COMMAND /usr/bin/codesign --verify --strict
                "$<TARGET_BUNDLE_DIR:talas_component_activation_sandbox_integration_tests>/Contents/XPCServices/TalasComponentProbe.xpc"
            COMMAND /usr/bin/codesign --verify --strict
                "$<TARGET_BUNDLE_DIR:talas_component_activation_sandbox_integration_tests>/Contents/XPCServices/TalasComponentHost.xpc"
            COMMAND /usr/bin/codesign --force --sign - --timestamp=none
                "$<TARGET_BUNDLE_DIR:talas_component_activation_sandbox_integration_tests>"
            VERBATIM)
        add_test(
            NAME talas_component_activation_sandbox_integration_tests
            COMMAND
                $<TARGET_FILE:talas_component_activation_sandbox_integration_tests>)
        set_tests_properties(
            talas_component_activation_sandbox_integration_tests
            PROPERTIES RUN_SERIAL TRUE TIMEOUT 20)

        add_executable(talas_component_bundle_probe_macos_coordinator_tests
            tests/runtime/ComponentBundleProbeMacOSCoordinatorTests.cpp)
        target_link_libraries(
            talas_component_bundle_probe_macos_coordinator_tests
            PRIVATE talas_component_probe_macos_coordinator)
        add_test(
            NAME talas_component_bundle_probe_macos_coordinator_tests
            COMMAND talas_component_bundle_probe_macos_coordinator_tests)

        add_executable(talas_component_bundle_probe_materialization_tests
            tests/runtime/ComponentBundleProbeMaterializationTests.cpp)
        target_link_libraries(
            talas_component_bundle_probe_materialization_tests
            PRIVATE talas_component_probe_macos_materialization)
        add_test(
            NAME talas_component_bundle_probe_materialization_tests
            COMMAND talas_component_bundle_probe_materialization_tests)

        add_library(talas_component_bundle_probe_unlinked_image_fixture MODULE
            tests/runtime/fixtures/ComponentBundleProbeCleanFixture.cpp)
        target_include_directories(
            talas_component_bundle_probe_unlinked_image_fixture
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_compile_features(
            talas_component_bundle_probe_unlinked_image_fixture PRIVATE cxx_std_20)
        target_compile_definitions(
            talas_component_bundle_probe_unlinked_image_fixture
            PRIVATE TALAS_COMPONENT_PROBE_FIXTURE_REQUIRES_UNLINKED_IMAGE=1)
        set_target_properties(
            talas_component_bundle_probe_unlinked_image_fixture
            PROPERTIES
                PREFIX ""
                OUTPUT_NAME "TalasComponentProbeUnlinkedImageFixture")
        add_custom_command(
            TARGET talas_component_bundle_probe_unlinked_image_fixture
            POST_BUILD
            COMMAND /usr/bin/codesign --force
                --sign -
                --timestamp=none
                "$<TARGET_FILE:talas_component_bundle_probe_unlinked_image_fixture>"
            VERBATIM)

        function(talas_add_component_probe_coordinator_transport_fixture
            target_name fixture_mode)
            add_executable(${target_name} MACOSX_BUNDLE
                tests/runtime/ComponentBundleProbeCoordinatorFixtureMain.cpp)
            target_include_directories(
                ${target_name}
                PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
            target_link_libraries(
                ${target_name}
                PRIVATE talas_component_probe_macos_xpc)
            target_compile_features(${target_name} PRIVATE cxx_std_20)
            target_compile_options(${target_name} PRIVATE -fblocks)
            target_compile_definitions(
                ${target_name}
                PRIVATE
                    TALAS_COMPONENT_PROBE_COORDINATOR_FIXTURE_MODE=${fixture_mode})
            set_target_properties(${target_name} PROPERTIES
                OUTPUT_NAME TalasComponentProbe
                RUNTIME_OUTPUT_DIRECTORY
                    "${CMAKE_CURRENT_BINARY_DIR}/component-probe-coordinator-fixtures/${target_name}"
                BUNDLE_EXTENSION xpc
                MACOSX_BUNDLE_INFO_PLIST
                    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/TalasComponentProbe-Info.plist"
                MACOSX_BUNDLE_GUI_IDENTIFIER
                    "com.talas.workbench.component-probe"
                MACOSX_BUNDLE_BUNDLE_NAME
                    "Talas Component Probe Coordinator Fixture"
                MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
                MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
            talas_sign_component_probe_helper(${target_name})
        endfunction()
        talas_add_component_probe_coordinator_transport_fixture(
            talas_component_probe_coordinator_malformed_fixture
            1)
        talas_add_component_probe_coordinator_transport_fixture(
            talas_component_probe_coordinator_delayed_fixture
            2)
        talas_add_component_probe_coordinator_transport_fixture(
            talas_component_probe_coordinator_late_fixture
            3)
        talas_add_component_probe_coordinator_transport_fixture(
            talas_component_probe_coordinator_no_start_fixture
            4)
        talas_add_component_probe_coordinator_transport_fixture(
            talas_component_probe_coordinator_wrong_process_fixture
            5)
        talas_add_component_probe_coordinator_transport_fixture(
            talas_component_probe_coordinator_post_reply_crash_fixture
            6)

        function(talas_add_component_probe_coordinator_integration_test
            target_name bundle_id scenario)
            set(helper_target talas_component_bundle_probe_helper)
            if(ARGC GREATER 3)
                set(helper_target "${ARGV3}")
            endif()
            add_executable(${target_name} MACOSX_BUNDLE
                tests/runtime/ComponentBundleProbeMacOSCoordinatorIntegrationTests.cpp)
            target_include_directories(
                ${target_name}
                PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
            target_link_libraries(
                ${target_name}
                PRIVATE talas_component_probe_macos_coordinator)
            target_compile_features(${target_name} PRIVATE cxx_std_20)
            target_compile_definitions(
                ${target_name}
                PRIVATE
                    TALAS_COMPONENT_PROBE_COORDINATOR_SCENARIO=${scenario}
                    "TALAS_COMPONENT_PROBE_CLEAN_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_bundle_probe_clean_fixture>\""
                    "TALAS_COMPONENT_PROBE_CONTROLLED_FATAL_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_bundle_probe_controlled_fatal_fixture>\""
                    "TALAS_COMPONENT_PROBE_MISSING_SYMBOL_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_bundle_probe_missing_symbol_fixture>\""
                    "TALAS_COMPONENT_PROBE_METADATA_MISMATCH_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_bundle_probe_metadata_mismatch_fixture>\""
                    "TALAS_COMPONENT_PROBE_CONSTRUCTOR_CRASH_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_bundle_probe_constructor_crash_fixture>\""
                    "TALAS_COMPONENT_PROBE_ENTRY_CRASH_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_bundle_probe_entry_crash_fixture>\""
                    "TALAS_COMPONENT_PROBE_ENTRY_HANG_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_bundle_probe_entry_hang_fixture>\""
                    "TALAS_COMPONENT_PROBE_UNLOAD_CRASH_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_bundle_probe_unload_crash_fixture>\""
                    "TALAS_COMPONENT_PROBE_UNLOAD_HANG_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_bundle_probe_unload_hang_fixture>\"")
            set_target_properties(${target_name} PROPERTIES
                MACOSX_BUNDLE_GUI_IDENTIFIER "${bundle_id}"
                MACOSX_BUNDLE_BUNDLE_NAME
                    "Talas Component Probe Coordinator Test")
            add_dependencies(
                ${target_name}
                talas_component_bundle_probe_clean_fixture
                talas_component_bundle_probe_controlled_fatal_fixture
                talas_component_bundle_probe_missing_symbol_fixture
                talas_component_bundle_probe_metadata_mismatch_fixture
                talas_component_bundle_probe_constructor_crash_fixture
                talas_component_bundle_probe_entry_crash_fixture
                talas_component_bundle_probe_entry_hang_fixture
                talas_component_bundle_probe_unload_crash_fixture
                talas_component_bundle_probe_unload_hang_fixture)
            if(helper_target STREQUAL "NO_HELPER")
                add_custom_command(
                    TARGET ${target_name}
                    POST_BUILD
                    COMMAND /usr/bin/codesign --force --sign - --timestamp=none
                        "$<TARGET_BUNDLE_DIR:${target_name}>"
                    VERBATIM)
            else()
                add_dependencies(${target_name} ${helper_target})
                set_property(
                    TARGET ${target_name}
                    APPEND PROPERTY LINK_DEPENDS
                        "$<TARGET_FILE:${helper_target}>")
                add_custom_command(
                    TARGET ${target_name}
                    POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        "$<TARGET_BUNDLE_DIR:${target_name}>/Contents/XPCServices"
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "$<TARGET_BUNDLE_DIR:${helper_target}>"
                        "$<TARGET_BUNDLE_DIR:${target_name}>/Contents/XPCServices/TalasComponentProbe.xpc"
                    COMMAND /usr/bin/codesign --force --sign - --timestamp=none
                        "$<TARGET_BUNDLE_DIR:${target_name}>/Contents/XPCServices/TalasComponentProbe.xpc"
                    COMMAND /usr/bin/codesign --force --sign - --timestamp=none
                        "$<TARGET_BUNDLE_DIR:${target_name}>"
                    VERBATIM)
            endif()
            add_test(NAME ${target_name} COMMAND $<TARGET_FILE:${target_name}>)
            set_tests_properties(
                ${target_name}
                PROPERTIES RUN_SERIAL TRUE TIMEOUT 15)
        endfunction()
        talas_add_component_probe_coordinator_integration_test(
            talas_component_probe_coordinator_clean_tests
            com.talas.component-probe-coordinator-clean-tests
            1)
        talas_add_component_probe_coordinator_integration_test(
            talas_component_probe_coordinator_image_reject_tests
            com.talas.component-probe-coordinator-image-tests
            2)
        talas_add_component_probe_coordinator_integration_test(
            talas_component_probe_coordinator_symbol_reject_tests
            com.talas.component-probe-coordinator-symbol-tests
            3)
        talas_add_component_probe_coordinator_integration_test(
            talas_component_probe_coordinator_abi_reject_tests
            com.talas.component-probe-coordinator-abi-tests
            4)
        talas_add_component_probe_coordinator_integration_test(
            talas_component_probe_coordinator_controlled_fatal_tests
            com.talas.component-probe-coordinator-fatal-tests
            5)
        talas_add_component_probe_coordinator_integration_test(
            talas_component_probe_coordinator_constructor_crash_tests
            com.talas.component-probe-coordinator-constructor-tests
            6)
        talas_add_component_probe_coordinator_integration_test(
            talas_component_probe_coordinator_entry_crash_tests
            com.talas.component-probe-coordinator-entry-crash-tests
            7)
        talas_add_component_probe_coordinator_integration_test(
            talas_component_probe_coordinator_entry_hang_tests
            com.talas.component-probe-coordinator-entry-hang-tests
            8)
        talas_add_component_probe_coordinator_integration_test(
            talas_component_probe_coordinator_unload_crash_tests
            com.talas.component-probe-coordinator-unload-crash-tests
            9)
        talas_add_component_probe_coordinator_integration_test(
            talas_component_probe_coordinator_unload_hang_tests
            com.talas.component-probe-coordinator-unload-hang-tests
            10)
        talas_add_component_probe_coordinator_integration_test(
            talas_component_probe_coordinator_malformed_tests
            com.talas.component-probe-coordinator-malformed-tests
            11
            talas_component_probe_coordinator_malformed_fixture)
        talas_add_component_probe_coordinator_integration_test(
            talas_component_probe_coordinator_launch_denied_tests
            com.talas.component-probe-coordinator-launch-denied-tests
            12
            NO_HELPER)
        talas_add_component_probe_coordinator_integration_test(
            talas_component_probe_coordinator_revocation_tests
            com.talas.component-probe-coordinator-revocation-tests
            13
            talas_component_probe_coordinator_delayed_fixture)
        talas_add_component_probe_coordinator_integration_test(
            talas_component_probe_coordinator_cancellation_tests
            com.talas.component-probe-coordinator-cancellation-tests
            14
            talas_component_probe_coordinator_delayed_fixture)
        talas_add_component_probe_coordinator_integration_test(
            talas_component_probe_coordinator_destruction_tests
            com.talas.component-probe-coordinator-destruction-tests
            15
            talas_component_probe_coordinator_delayed_fixture)
        talas_add_component_probe_coordinator_integration_test(
            talas_component_probe_coordinator_late_reply_tests
            com.talas.component-probe-coordinator-late-reply-tests
            16
            talas_component_probe_coordinator_late_fixture)
        talas_add_component_probe_coordinator_integration_test(
            talas_component_probe_coordinator_no_start_tests
            com.talas.component-probe-coordinator-no-start-tests
            17
            talas_component_probe_coordinator_no_start_fixture)
        talas_add_component_probe_coordinator_integration_test(
            talas_component_probe_coordinator_wrong_process_tests
            com.talas.component-probe-coordinator-wrong-process-tests
            18
            talas_component_probe_coordinator_wrong_process_fixture)
        talas_add_component_probe_coordinator_integration_test(
            talas_component_probe_coordinator_post_reply_crash_tests
            com.talas.component-probe-coordinator-post-reply-crash-tests
            19
            talas_component_probe_coordinator_post_reply_crash_fixture)

        add_executable(talas_component_bundle_probe_helper_integration_tests
            MACOSX_BUNDLE
            tests/runtime/ComponentBundleProbeHelperIntegrationTests.cpp)
        target_include_directories(
            talas_component_bundle_probe_helper_integration_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(
            talas_component_bundle_probe_helper_integration_tests
            PRIVATE
                talas_component_probe_macos_xpc
                "-framework Security"
                "-framework CoreFoundation")
        target_compile_features(
            talas_component_bundle_probe_helper_integration_tests
            PRIVATE cxx_std_20)
        target_compile_options(
            talas_component_bundle_probe_helper_integration_tests
            PRIVATE -fblocks)
        target_compile_definitions(
            talas_component_bundle_probe_helper_integration_tests
            PRIVATE
                "TALAS_COMPONENT_PROBE_CLEAN_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_bundle_probe_clean_fixture>\""
                "TALAS_COMPONENT_PROBE_CONTROLLED_FATAL_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_bundle_probe_controlled_fatal_fixture>\""
                "TALAS_COMPONENT_PROBE_UNLINKED_IMAGE_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_bundle_probe_unlinked_image_fixture>\"")
        set_target_properties(
            talas_component_bundle_probe_helper_integration_tests
            PROPERTIES
                MACOSX_BUNDLE_GUI_IDENTIFIER
                    "com.talas.component-probe-helper-tests"
                MACOSX_BUNDLE_BUNDLE_NAME
                    "Talas Component Probe Helper Tests")
        add_dependencies(
            talas_component_bundle_probe_helper_integration_tests
            talas_component_bundle_probe_helper
            talas_component_bundle_probe_clean_fixture
            talas_component_bundle_probe_controlled_fatal_fixture
            talas_component_bundle_probe_unlinked_image_fixture)
        set_property(
            TARGET talas_component_bundle_probe_helper_integration_tests
            APPEND PROPERTY LINK_DEPENDS
                "$<TARGET_FILE:talas_component_bundle_probe_helper>")
        add_custom_command(
            TARGET talas_component_bundle_probe_helper_integration_tests
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper_integration_tests>/Contents/XPCServices"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper>"
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper_integration_tests>/Contents/XPCServices/TalasComponentProbe.xpc"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper>"
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper_integration_tests>/Contents/XPCServices/TalasComponentProbeCleanFixture.xpc"
            COMMAND /usr/bin/plutil -replace CFBundleIdentifier -string
                "com.talas.workbench.component-probe.fixture.local-clean-v1"
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper_integration_tests>/Contents/XPCServices/TalasComponentProbeCleanFixture.xpc/Contents/Info.plist"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper>"
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper_integration_tests>/Contents/XPCServices/TalasComponentProbeDigestFixture.xpc"
            COMMAND /usr/bin/plutil -replace CFBundleIdentifier -string
                "com.talas.workbench.component-probe.fixture.local-digest-v1"
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper_integration_tests>/Contents/XPCServices/TalasComponentProbeDigestFixture.xpc/Contents/Info.plist"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper>"
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper_integration_tests>/Contents/XPCServices/TalasComponentProbeAbiFixture.xpc"
            COMMAND /usr/bin/plutil -replace CFBundleIdentifier -string
                "com.talas.workbench.component-probe.fixture.local-abi-v1"
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper_integration_tests>/Contents/XPCServices/TalasComponentProbeAbiFixture.xpc/Contents/Info.plist"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper>"
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper_integration_tests>/Contents/XPCServices/TalasComponentProbeFatalFixture.xpc"
            COMMAND /usr/bin/plutil -replace CFBundleIdentifier -string
                "com.talas.workbench.component-probe.fixture.local-fatal-v1"
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper_integration_tests>/Contents/XPCServices/TalasComponentProbeFatalFixture.xpc/Contents/Info.plist"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper>"
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper_integration_tests>/Contents/XPCServices/TalasComponentProbeUnlinkedFixture.xpc"
            COMMAND /usr/bin/plutil -replace CFBundleIdentifier -string
                "com.talas.workbench.component-probe.fixture.local-unlinked-v1"
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper_integration_tests>/Contents/XPCServices/TalasComponentProbeUnlinkedFixture.xpc/Contents/Info.plist"
            COMMAND /usr/bin/codesign --force
                --sign -
                --timestamp=none
                --entitlements
                    "${TALAS_COMPONENT_PROBE_HELPER_ENTITLEMENTS}"
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper_integration_tests>/Contents/XPCServices/TalasComponentProbe.xpc"
            COMMAND /usr/bin/codesign --force
                --sign -
                --timestamp=none
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper_integration_tests>/Contents/XPCServices/TalasComponentProbeCleanFixture.xpc"
            COMMAND /usr/bin/codesign --force
                --sign -
                --timestamp=none
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper_integration_tests>/Contents/XPCServices/TalasComponentProbeDigestFixture.xpc"
            COMMAND /usr/bin/codesign --force
                --sign -
                --timestamp=none
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper_integration_tests>/Contents/XPCServices/TalasComponentProbeAbiFixture.xpc"
            COMMAND /usr/bin/codesign --force
                --sign -
                --timestamp=none
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper_integration_tests>/Contents/XPCServices/TalasComponentProbeFatalFixture.xpc"
            COMMAND /usr/bin/codesign --force
                --sign -
                --timestamp=none
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper_integration_tests>/Contents/XPCServices/TalasComponentProbeUnlinkedFixture.xpc"
            COMMAND /usr/bin/codesign --force
                --sign -
                --timestamp=none
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper_integration_tests>"
            VERBATIM)
        add_test(
            NAME talas_component_bundle_probe_helper_integration_tests
            COMMAND
                $<TARGET_FILE:talas_component_bundle_probe_helper_integration_tests>)
        set_tests_properties(
            talas_component_bundle_probe_helper_integration_tests
            PROPERTIES RUN_SERIAL TRUE TIMEOUT 30)

        add_executable(talas_component_bundle_probe_sandbox_fixture MACOSX_BUNDLE
            tests/runtime/ComponentBundleProbeSandboxFixtureMain.cpp
            tests/runtime/ComponentBundleProbeSandboxProbe.cpp)
        target_include_directories(
            talas_component_bundle_probe_sandbox_fixture
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_features(
            talas_component_bundle_probe_sandbox_fixture
            PRIVATE cxx_std_20)
        target_compile_options(
            talas_component_bundle_probe_sandbox_fixture
            PRIVATE -fblocks)
        target_link_libraries(
            talas_component_bundle_probe_sandbox_fixture
            PRIVATE
                "-framework Security"
                "-framework CoreFoundation")
        target_compile_definitions(
            talas_component_bundle_probe_sandbox_fixture
            PRIVATE
                "TALAS_COMPONENT_PROBE_SANDBOX_SOURCE_BUNDLE_PATH=\"$<TARGET_FILE:talas_component_bundle_probe_clean_fixture>\"")
        set_target_properties(
            talas_component_bundle_probe_sandbox_fixture
            PROPERTIES
                OUTPUT_NAME "TalasComponentProbeSandboxFixture"
                BUNDLE_EXTENSION "xpc"
                MACOSX_BUNDLE_INFO_PLIST
                    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/TalasComponentProbeSandboxFixture-Info.plist")
        add_dependencies(
            talas_component_bundle_probe_sandbox_fixture
            talas_component_bundle_probe_clean_fixture)
        talas_sign_component_probe_helper(
            talas_component_bundle_probe_sandbox_fixture)

        add_executable(talas_component_bundle_probe_sandbox_integration_tests
            MACOSX_BUNDLE
            tests/runtime/ComponentBundleProbeSandboxIntegrationTests.cpp)
        target_include_directories(
            talas_component_bundle_probe_sandbox_integration_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(
            talas_component_bundle_probe_sandbox_integration_tests
            PRIVATE
                "-framework Security"
                "-framework CoreFoundation")
        target_compile_features(
            talas_component_bundle_probe_sandbox_integration_tests
            PRIVATE cxx_std_20)
        target_compile_options(
            talas_component_bundle_probe_sandbox_integration_tests
            PRIVATE -fblocks)
        set_target_properties(
            talas_component_bundle_probe_sandbox_integration_tests
            PROPERTIES
                MACOSX_BUNDLE_GUI_IDENTIFIER
                    "com.talas.component-probe-sandbox-tests"
                MACOSX_BUNDLE_BUNDLE_NAME
                    "Talas Component Probe Sandbox Tests")
        add_dependencies(
            talas_component_bundle_probe_sandbox_integration_tests
            talas_component_bundle_probe_sandbox_fixture
            talas_component_isolation_helper)
        set_property(
            TARGET talas_component_bundle_probe_sandbox_integration_tests
            APPEND PROPERTY LINK_DEPENDS
                "$<TARGET_FILE:talas_component_bundle_probe_sandbox_fixture>"
                "$<TARGET_FILE:talas_component_isolation_helper>")
        add_custom_command(
            TARGET talas_component_bundle_probe_sandbox_integration_tests
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_sandbox_integration_tests>/Contents/XPCServices"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_sandbox_fixture>"
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_sandbox_integration_tests>/Contents/XPCServices/TalasComponentProbeSandboxFixture.xpc"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "$<TARGET_BUNDLE_DIR:talas_component_isolation_helper>"
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_sandbox_integration_tests>/Contents/XPCServices/TalasComponentHost.xpc"
            COMMAND /usr/bin/codesign --force --sign - --timestamp=none
                --entitlements
                    "${TALAS_COMPONENT_PROBE_HELPER_ENTITLEMENTS}"
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_sandbox_integration_tests>/Contents/XPCServices/TalasComponentProbeSandboxFixture.xpc"
            COMMAND /usr/bin/codesign --force --sign - --timestamp=none
                --entitlements
                    "${TALAS_COMPONENT_ISOLATION_HELPER_ENTITLEMENTS}"
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_sandbox_integration_tests>/Contents/XPCServices/TalasComponentHost.xpc"
            COMMAND /usr/bin/codesign --force --sign - --timestamp=none
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_sandbox_integration_tests>"
            VERBATIM)
        add_test(
            NAME talas_component_bundle_probe_sandbox_integration_tests
            COMMAND
                $<TARGET_FILE:talas_component_bundle_probe_sandbox_integration_tests>)
        set_tests_properties(
            talas_component_bundle_probe_sandbox_integration_tests
            PROPERTIES RUN_SERIAL TRUE TIMEOUT 20)

        function(talas_embed_component_isolation_test_helper
            host_target helper_target helper_bundle_name)
            add_dependencies(${host_target} ${helper_target})
            set_property(
                TARGET ${host_target}
                APPEND PROPERTY LINK_DEPENDS
                    "$<TARGET_FILE:${helper_target}>")
            add_custom_command(
                TARGET ${host_target}
                POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_BUNDLE_DIR:${host_target}>/Contents/XPCServices"
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "$<TARGET_BUNDLE_DIR:${helper_target}>"
                    "$<TARGET_BUNDLE_DIR:${host_target}>/Contents/XPCServices/${helper_bundle_name}.xpc"
                COMMAND /usr/bin/codesign --force --sign - --timestamp=none
                    --entitlements
                        "${TALAS_COMPONENT_ISOLATION_HELPER_ENTITLEMENTS}"
                    "$<TARGET_BUNDLE_DIR:${host_target}>/Contents/XPCServices/${helper_bundle_name}.xpc"
                VERBATIM)
        endfunction()

        set(TALAS_COMPONENT_ISOLATION_TEST_HELPER_TARGETS
            talas_component_isolation_helper
            talas_component_isolation_helper_malformed
            talas_component_isolation_helper_wrong_version
            talas_component_isolation_helper_wrong_nonce
            talas_component_isolation_helper_stale_generation
            talas_component_isolation_helper_controlled_failure
            talas_component_isolation_helper_crash
            talas_component_isolation_helper_hang
            talas_component_isolation_helper_oversized_diagnostic
            talas_component_isolation_helper_early_exit
            talas_component_isolation_helper_teardown_race)
        set(TALAS_COMPONENT_ISOLATION_TEST_HELPER_BUNDLES
            TalasComponentHost
            TalasComponentHostMalformed
            TalasComponentHostWrongVersion
            TalasComponentHostWrongNonce
            TalasComponentHostStaleGeneration
            TalasComponentHostControlledFailure
            TalasComponentHostCrash
            TalasComponentHostHang
            TalasComponentHostOversizedDiagnostic
            TalasComponentHostEarlyExit
            TalasComponentHostTeardownRace)

        function(talas_add_component_isolation_host_integration_test
            target_name bundle_identifier fixture_group)
            add_executable(${target_name} MACOSX_BUNDLE
                tests/runtime/ComponentIsolationMacOSHostIntegrationTests.cpp)
            target_include_directories(${target_name}
                PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
            target_link_libraries(${target_name}
                PRIVATE talas_component_isolation_macos_host)
            target_compile_features(${target_name} PRIVATE cxx_std_20)
            target_compile_definitions(${target_name}
                PRIVATE
                    TALAS_COMPONENT_ISOLATION_INTEGRATION_GROUP=${fixture_group})
            set_target_properties(${target_name} PROPERTIES
                MACOSX_BUNDLE_GUI_IDENTIFIER "${bundle_identifier}"
                MACOSX_BUNDLE_BUNDLE_NAME
                    "Talas Component Host Integration Tests")
            foreach(helper_target helper_bundle IN ZIP_LISTS
                TALAS_COMPONENT_ISOLATION_TEST_HELPER_TARGETS
                TALAS_COMPONENT_ISOLATION_TEST_HELPER_BUNDLES)
                talas_embed_component_isolation_test_helper(
                    ${target_name} ${helper_target} ${helper_bundle})
            endforeach()
            add_custom_command(
                TARGET ${target_name}
                POST_BUILD
                COMMAND /usr/bin/codesign --force --sign - --timestamp=none
                    "$<TARGET_BUNDLE_DIR:${target_name}>"
                VERBATIM)
            add_test(NAME ${target_name} COMMAND $<TARGET_FILE:${target_name}>)
            set_tests_properties(${target_name}
                PROPERTIES RUN_SERIAL TRUE TIMEOUT 15)
        endfunction()

        talas_add_component_isolation_host_integration_test(
            talas_component_isolation_macos_host_integration_tests
            com.talas.component-host-integration-tests
            1)
        talas_add_component_isolation_host_integration_test(
            talas_component_isolation_macos_host_failure_tests
            com.talas.component-host-failure-tests
            2)

        add_executable(talas_component_probe_trust_composition_tests MACOSX_BUNDLE
            tests/runtime/ComponentBundleProbeTrustCompositionIntegrationTests.cpp)
        target_include_directories(
            talas_component_probe_trust_composition_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(
            talas_component_probe_trust_composition_tests
            PRIVATE
                talas_component_probe_macos_coordinator
                talas_component_isolation_macos_host)
        target_compile_features(
            talas_component_probe_trust_composition_tests PRIVATE cxx_std_20)
        target_compile_definitions(
            talas_component_probe_trust_composition_tests
            PRIVATE
                "TALAS_COMPONENT_PROBE_CLEAN_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_bundle_probe_clean_fixture>\"")
        set_target_properties(
            talas_component_probe_trust_composition_tests
            PROPERTIES
                MACOSX_BUNDLE_GUI_IDENTIFIER
                    "com.talas.component-probe-trust-composition-tests"
                MACOSX_BUNDLE_BUNDLE_NAME
                    "Talas Component Probe Trust Composition Tests")
        add_dependencies(
            talas_component_probe_trust_composition_tests
            talas_component_bundle_probe_clean_fixture)
        talas_embed_component_isolation_test_helper(
            talas_component_probe_trust_composition_tests
            talas_component_isolation_helper
            TalasComponentHost)
        add_dependencies(
            talas_component_probe_trust_composition_tests
            talas_component_bundle_probe_helper)
        set_property(
            TARGET talas_component_probe_trust_composition_tests
            APPEND PROPERTY LINK_DEPENDS
                "$<TARGET_FILE:talas_component_bundle_probe_helper>")
        add_custom_command(
            TARGET talas_component_probe_trust_composition_tests
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_DIR:talas_component_probe_trust_composition_tests>/Contents/XPCServices"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper>"
                "$<TARGET_BUNDLE_DIR:talas_component_probe_trust_composition_tests>/Contents/XPCServices/TalasComponentProbe.xpc"
            # The hermetic module fixture is ad-hoc signed, so this composition
            # test uses the established unsandboxed loader fixture profile.
            # The exact production probe sandbox is verified separately.
            COMMAND /usr/bin/codesign --force --sign - --timestamp=none
                "$<TARGET_BUNDLE_DIR:talas_component_probe_trust_composition_tests>/Contents/XPCServices/TalasComponentProbe.xpc"
            COMMAND /usr/bin/codesign --force --sign - --timestamp=none
                "$<TARGET_BUNDLE_DIR:talas_component_probe_trust_composition_tests>"
            VERBATIM)
        add_test(
            NAME talas_component_probe_trust_composition_tests
            COMMAND $<TARGET_FILE:talas_component_probe_trust_composition_tests>)
        set_tests_properties(
            talas_component_probe_trust_composition_tests
            PROPERTIES RUN_SERIAL TRUE TIMEOUT 20)

        add_executable(
            talas_component_bundle_replacement_controller_integration_tests
            MACOSX_BUNDLE
            tests/runtime/ComponentBundleReplacementControllerIntegrationTests.cpp)
        target_include_directories(
            talas_component_bundle_replacement_controller_integration_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(
            talas_component_bundle_replacement_controller_integration_tests
            PRIVATE talas_component_bundle_replacement_controller)
        target_compile_features(
            talas_component_bundle_replacement_controller_integration_tests
            PRIVATE cxx_std_20)
        target_compile_definitions(
            talas_component_bundle_replacement_controller_integration_tests
            PRIVATE
                "TALAS_COMPONENT_REPLACEMENT_FIXTURE_PATH=\"$<TARGET_FILE:talas_component_replacement_gain_fixture>\"")
        set_target_properties(
            talas_component_bundle_replacement_controller_integration_tests
            PROPERTIES
                MACOSX_BUNDLE_GUI_IDENTIFIER
                    "com.talas.component-replacement-controller-tests"
                MACOSX_BUNDLE_BUNDLE_NAME
                    "Talas Component Replacement Controller Tests")
        add_dependencies(
            talas_component_bundle_replacement_controller_integration_tests
            talas_component_replacement_gain_fixture
            talas_component_bundle_probe_helper
            talas_component_activation_helper)
        set_property(
            TARGET talas_component_bundle_replacement_controller_integration_tests
            APPEND PROPERTY LINK_DEPENDS
                "$<TARGET_FILE:talas_component_bundle_probe_helper>"
                "$<TARGET_FILE:talas_component_activation_helper>")
        add_custom_command(
            TARGET talas_component_bundle_replacement_controller_integration_tests
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_replacement_controller_integration_tests>/Contents/XPCServices"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_probe_helper>"
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_replacement_controller_integration_tests>/Contents/XPCServices/TalasComponentProbe.xpc"
            COMMAND /usr/bin/codesign --force --sign - --timestamp=none
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_replacement_controller_integration_tests>/Contents/XPCServices/TalasComponentProbe.xpc"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "$<TARGET_BUNDLE_DIR:talas_component_activation_helper>"
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_replacement_controller_integration_tests>/Contents/XPCServices/TalasComponentRuntime.xpc"
            COMMAND /usr/bin/codesign --verify --strict
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_replacement_controller_integration_tests>/Contents/XPCServices/TalasComponentRuntime.xpc"
            COMMAND /usr/bin/codesign --force --sign - --timestamp=none
                "$<TARGET_BUNDLE_DIR:talas_component_bundle_replacement_controller_integration_tests>"
            VERBATIM)
        add_test(
            NAME talas_component_bundle_replacement_controller_integration_tests
            COMMAND
                $<TARGET_FILE:talas_component_bundle_replacement_controller_integration_tests>)
        set_tests_properties(
            talas_component_bundle_replacement_controller_integration_tests
            PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)

        add_executable(talas_component_isolation_sandbox_integration_tests
            MACOSX_BUNDLE
            tests/runtime/ComponentIsolationSandboxIntegrationTests.cpp)
        target_include_directories(
            talas_component_isolation_sandbox_integration_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(
            talas_component_isolation_sandbox_integration_tests
            PRIVATE
                talas_component_isolation_macos_host
                "-framework Security"
                "-framework CoreFoundation")
        target_compile_features(
            talas_component_isolation_sandbox_integration_tests
            PRIVATE cxx_std_20)
        set_target_properties(
            talas_component_isolation_sandbox_integration_tests
            PROPERTIES
                MACOSX_BUNDLE_GUI_IDENTIFIER
                    "com.talas.component-host-sandbox-tests"
                MACOSX_BUNDLE_BUNDLE_NAME
                    "Talas Component Host Sandbox Tests")
        talas_embed_component_isolation_test_helper(
            talas_component_isolation_sandbox_integration_tests
            talas_component_isolation_helper_sandbox
            TalasComponentHostSandbox)
        add_dependencies(
            talas_component_isolation_sandbox_integration_tests
            talas_component_isolation_helper)
        add_custom_command(
            TARGET talas_component_isolation_sandbox_integration_tests
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "$<TARGET_BUNDLE_DIR:talas_component_isolation_helper>"
                "$<TARGET_BUNDLE_DIR:talas_component_isolation_sandbox_integration_tests>/Contents/XPCServices/TalasComponentHostUnsandboxed.xpc"
            COMMAND /usr/bin/codesign --force --sign - --timestamp=none
                "$<TARGET_BUNDLE_DIR:talas_component_isolation_sandbox_integration_tests>/Contents/XPCServices/TalasComponentHostUnsandboxed.xpc"
            VERBATIM)
        add_custom_command(
            TARGET talas_component_isolation_sandbox_integration_tests
            POST_BUILD
            COMMAND /usr/bin/codesign --force --sign - --timestamp=none
                "$<TARGET_BUNDLE_DIR:talas_component_isolation_sandbox_integration_tests>"
            VERBATIM)
        add_test(
            NAME talas_component_isolation_sandbox_integration_tests
            COMMAND
                $<TARGET_FILE:talas_component_isolation_sandbox_integration_tests>)
        set_tests_properties(
            talas_component_isolation_sandbox_integration_tests
            PROPERTIES RUN_SERIAL TRUE TIMEOUT 15)

        add_executable(talas_component_isolation_watchdog_integration_tests
            MACOSX_BUNDLE
            tests/runtime/ComponentIsolationMacOSHostIntegrationTests.cpp)
        target_link_libraries(
            talas_component_isolation_watchdog_integration_tests
            PRIVATE talas_component_isolation_macos_host)
        target_compile_features(
            talas_component_isolation_watchdog_integration_tests
            PRIVATE cxx_std_20)
        target_compile_definitions(
            talas_component_isolation_watchdog_integration_tests
            PRIVATE TALAS_COMPONENT_ISOLATION_INTEGRATION_GROUP=3)
        set_target_properties(
            talas_component_isolation_watchdog_integration_tests
            PROPERTIES
                MACOSX_BUNDLE_GUI_IDENTIFIER
                    "com.talas.component-host-watchdog-tests"
                MACOSX_BUNDLE_BUNDLE_NAME
                    "Talas Component Host Watchdog Tests")
        talas_embed_component_isolation_test_helper(
            talas_component_isolation_watchdog_integration_tests
            talas_component_isolation_helper_hang
            TalasComponentHostHang)
        talas_embed_component_isolation_test_helper(
            talas_component_isolation_watchdog_integration_tests
            talas_component_isolation_helper_heartbeat_hang
            TalasComponentHostHeartbeatHang)
        talas_embed_component_isolation_test_helper(
            talas_component_isolation_watchdog_integration_tests
            talas_component_isolation_helper_teardown_hang
            TalasComponentHostTeardownHang)
        talas_embed_component_isolation_test_helper(
            talas_component_isolation_watchdog_integration_tests
            talas_component_isolation_helper_heartbeat_late
            TalasComponentHostHeartbeatLate)
        talas_embed_component_isolation_test_helper(
            talas_component_isolation_watchdog_integration_tests
            talas_component_isolation_helper_prewarm_hang
            TalasComponentHostPrewarmHang)
        add_custom_command(
            TARGET talas_component_isolation_watchdog_integration_tests
            POST_BUILD
            COMMAND /usr/bin/codesign --force --sign - --timestamp=none
                "$<TARGET_BUNDLE_DIR:talas_component_isolation_watchdog_integration_tests>"
            VERBATIM)
        add_test(
            NAME talas_component_isolation_watchdog_integration_tests
            COMMAND
                $<TARGET_FILE:talas_component_isolation_watchdog_integration_tests>)
        set_tests_properties(
            talas_component_isolation_watchdog_integration_tests
            PROPERTIES RUN_SERIAL TRUE TIMEOUT 15)
    endif()

    add_executable(talas_typed_event_tests
        tests/runtime/TypedEventTests.cpp
    )
    target_link_libraries(talas_typed_event_tests PRIVATE talas_runtime)
    add_test(NAME talas_typed_event_tests COMMAND talas_typed_event_tests)
    set_tests_properties(
        talas_typed_event_tests PROPERTIES LABELS "property")

    add_executable(talas_nested_sound_runtime_tests
        tests/runtime/NestedSoundRuntimeTests.cpp
    )
    target_link_libraries(talas_nested_sound_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_nested_sound_runtime_tests
        COMMAND talas_nested_sound_runtime_tests
    )

    add_executable(talas_realtime_render_session_tests
        tests/runtime/RealtimeRenderSessionTests.cpp
    )
    target_include_directories(talas_realtime_render_session_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(talas_realtime_render_session_tests PRIVATE talas_runtime)
    # At the core-tsan preset's -O1 this 107,000-line source alone takes over
    # half an hour of CPU to compile, against 18 s at -O0, while the runtime
    # libraries it calls (built at -O1) are what its run time is spent in.
    if(CMAKE_CXX_FLAGS MATCHES "-fsanitize=[^ ]*thread")
        set_property(SOURCE tests/runtime/RealtimeRenderSessionTests.cpp
            APPEND PROPERTY COMPILE_OPTIONS -O0)
    endif()
    # The full case list runs as eight ctest entries, each taking every
    # eighth case (--shard=K/8), so ctest -j spreads it, under TSan too.
    set(talas_realtime_render_session_tests_shards)
    foreach(talas_shard RANGE 7)
        add_test(NAME talas_realtime_render_session_tests_shard_${talas_shard}
            COMMAND talas_realtime_render_session_tests --shard=${talas_shard}/8)
        list(APPEND talas_realtime_render_session_tests_shards
            talas_realtime_render_session_tests_shard_${talas_shard})
    endforeach()
    add_test(NAME talas_delay_checkpoint_coexistence_tests
        COMMAND talas_realtime_render_session_tests --delay-checkpoint-coexistence)

    add_executable(talas_phase_shift_by_90_reference_tests
        tests/runtime/PhaseShiftBy90ReferenceTests.cpp)
    target_link_libraries(talas_phase_shift_by_90_reference_tests PRIVATE talas_runtime)
    add_test(NAME talas_phase_shift_by_90_reference_tests
        COMMAND talas_phase_shift_by_90_reference_tests)

    add_executable(talas_single_side_band_rm_reference_tests
        tests/runtime/SingleSideBandRmReferenceTests.cpp)
    target_link_libraries(talas_single_side_band_rm_reference_tests PRIVATE talas_runtime)
    add_test(NAME talas_single_side_band_rm_reference_tests
        COMMAND talas_single_side_band_rm_reference_tests)

    add_executable(talas_analysis_filter_reference_tests
        tests/runtime/AnalysisFilterReferenceTests.cpp)
    target_link_libraries(talas_analysis_filter_reference_tests PRIVATE talas_runtime)
    add_test(NAME talas_analysis_filter_reference_tests
        COMMAND talas_analysis_filter_reference_tests)

    add_executable(talas_two_formant_reference_tests
        tests/runtime/TwoFormantReferenceTests.cpp)
    target_link_libraries(talas_two_formant_reference_tests PRIVATE talas_runtime)
    add_test(NAME talas_two_formant_reference_tests
        COMMAND talas_two_formant_reference_tests)

    add_executable(talas_formant_bank_reference_tests
        tests/runtime/FormantBankReferenceTests.cpp
        tests/runtime/FormantBankAssetReferenceTests.cpp)
    target_link_libraries(talas_formant_bank_reference_tests PRIVATE talas_runtime)
    add_test(NAME talas_formant_bank_reference_tests
        COMMAND talas_formant_bank_reference_tests)

    add_executable(talas_oscillator_bank_source_frame_reference_tests
        tests/runtime/OscillatorBankSourceFrameReferenceTests.cpp)
    target_link_libraries(talas_oscillator_bank_source_frame_reference_tests PRIVATE talas_runtime)
    add_test(NAME talas_oscillator_bank_source_frame_reference_tests
        COMMAND talas_oscillator_bank_source_frame_reference_tests)

    add_executable(talas_waveshaper_memory_writer_reference_tests
        tests/runtime/WaveshaperMemoryWriterReferenceTests.cpp)
    target_link_libraries(talas_waveshaper_memory_writer_reference_tests PRIVATE talas_runtime)
    add_test(NAME talas_waveshaper_memory_writer_reference_tests
        COMMAND talas_waveshaper_memory_writer_reference_tests)

    add_executable(talas_triggered_table_read_memory_writer_reference_tests
        tests/runtime/TriggeredTableReadMemoryWriterReferenceTests.cpp)
    target_link_libraries(talas_triggered_table_read_memory_writer_reference_tests PRIVATE talas_runtime)
    add_test(NAME talas_triggered_table_read_memory_writer_reference_tests
        COMMAND talas_triggered_table_read_memory_writer_reference_tests)

    add_executable(talas_time_frequency_scale_memory_writer_reference_tests
        tests/runtime/TimeFrequencyScaleMemoryWriterReferenceTests.cpp)
    target_link_libraries(talas_time_frequency_scale_memory_writer_reference_tests PRIVATE talas_runtime)
    add_test(NAME talas_time_frequency_scale_memory_writer_reference_tests
        COMMAND talas_time_frequency_scale_memory_writer_reference_tests)

    add_executable(talas_time_frequency_scale_disk_source_reference_tests
        tests/runtime/TimeFrequencyScaleDiskSourceReferenceTests.cpp)
    target_link_libraries(talas_time_frequency_scale_disk_source_reference_tests PRIVATE talas_runtime talas_audio)
    add_test(NAME talas_time_frequency_scale_disk_source_reference_tests
        COMMAND talas_time_frequency_scale_disk_source_reference_tests)

    add_executable(talas_time_frequency_scale_quarter_speed_reference_tests
        tests/runtime/TimeFrequencyScaleQuarterSpeedReferenceTests.cpp)
    target_link_libraries(talas_time_frequency_scale_quarter_speed_reference_tests PRIVATE talas_runtime)
    add_test(NAME talas_time_frequency_scale_quarter_speed_reference_tests
        COMMAND talas_time_frequency_scale_quarter_speed_reference_tests)

    add_executable(talas_realtime_control_ingress_tests
        tests/runtime/RealtimeControlIngressTests.cpp
    )
    target_link_libraries(talas_realtime_control_ingress_tests PRIVATE talas_runtime)
    add_test(NAME talas_realtime_control_ingress_tests COMMAND talas_realtime_control_ingress_tests)

    add_executable(talas_realtime_live_set_ingress_tests
        tests/runtime/RealtimeLiveSetIngressTests.cpp
    )
    target_link_libraries(talas_realtime_live_set_ingress_tests PRIVATE talas_runtime)
    add_test(NAME talas_realtime_live_set_ingress_tests COMMAND talas_realtime_live_set_ingress_tests)
    set_tests_properties(talas_realtime_live_set_ingress_tests PROPERTIES TIMEOUT 30)

    add_executable(talas_prepared_live_set_tests
        tests/runtime/PreparedLiveSetTests.cpp
    )
    target_link_libraries(talas_prepared_live_set_tests PRIVATE talas_runtime)
    add_test(NAME talas_prepared_live_set_tests COMMAND talas_prepared_live_set_tests)

    add_executable(talas_prepared_live_tools_tests
        tests/runtime/PreparedLiveToolsTests.cpp
    )
    target_link_libraries(talas_prepared_live_tools_tests PRIVATE talas_runtime)
    add_test(NAME talas_prepared_live_tools_tests COMMAND talas_prepared_live_tools_tests)

    add_executable(talas_realtime_live_tool_ingress_tests
        tests/runtime/RealtimeLiveToolIngressTests.cpp
    )
    target_link_libraries(talas_realtime_live_tool_ingress_tests PRIVATE talas_runtime)
    add_test(NAME talas_realtime_live_tool_ingress_tests COMMAND talas_realtime_live_tool_ingress_tests)

    add_executable(talas_realtime_midi_voice_allocator_tests
        tests/runtime/RealtimeMidiVoiceAllocatorTests.cpp
    )
    target_link_libraries(talas_realtime_midi_voice_allocator_tests PRIVATE talas_runtime)
    add_test(NAME talas_realtime_midi_voice_allocator_tests COMMAND talas_realtime_midi_voice_allocator_tests)

    add_executable(talas_offline_render_scaling_tests
        tests/runtime/OfflineRenderScalingTests.cpp
    )
    target_link_libraries(talas_offline_render_scaling_tests PRIVATE talas_runtime)
    add_test(NAME talas_offline_render_scaling_tests COMMAND talas_offline_render_scaling_tests)

    if(APPLE)
        # Test-only probe: counts realtime-unsafe calls on the rendering thread.
        add_library(talas_realtime_probe SHARED tests/support/RealtimeProbe.c)
        add_executable(talas_realtime_safety_tests
            tests/runtime/RealtimeSafetyTests.cpp
        )
        target_link_libraries(talas_realtime_safety_tests PRIVATE talas_runtime talas_audio)
        add_dependencies(talas_realtime_safety_tests talas_realtime_probe)
        add_test(
            NAME talas_realtime_safety_tests
            COMMAND talas_realtime_safety_tests
                ${CMAKE_CURRENT_SOURCE_DIR}/examples
        )
        set_tests_properties(talas_realtime_safety_tests PROPERTIES
            ENVIRONMENT "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:talas_realtime_probe>"
            LABELS "realtime-safety"
            TIMEOUT 600)
    endif()

    add_executable(talas_standard_midi_file_tests
        tests/runtime/StandardMidiFileTests.cpp
    )
    target_link_libraries(talas_standard_midi_file_tests PRIVATE talas_runtime)
    add_test(NAME talas_standard_midi_file_tests COMMAND talas_standard_midi_file_tests)

    add_executable(talas_midi_file_voice_playback_tests tests/runtime/MidiFileVoicePlaybackTests.cpp)
    target_link_libraries(talas_midi_file_voice_playback_tests PRIVATE talas_runtime)
    add_test(NAME talas_midi_file_voice_playback_tests COMMAND talas_midi_file_voice_playback_tests)

    add_executable(talas_midi_file_source_workflow_tests tests/runtime/MidiFileSourceWorkflowTests.cpp)
    target_link_libraries(talas_midi_file_source_workflow_tests PRIVATE talas_cli_lib)
    add_test(NAME talas_midi_file_source_workflow_tests COMMAND talas_midi_file_source_workflow_tests)

    add_executable(talas_prepared_midi_file_voice_sources_tests
        tests/runtime/PreparedMidiFileVoiceSourcesTests.cpp
    )
    target_link_libraries(talas_prepared_midi_file_voice_sources_tests PRIVATE talas_runtime)
    add_test(NAME talas_prepared_midi_file_voice_sources_tests COMMAND talas_prepared_midi_file_voice_sources_tests)

    add_executable(talas_prepared_midi_mapper_tests
        tests/runtime/PreparedMidiMapperTests.cpp
    )
    target_link_libraries(talas_prepared_midi_mapper_tests PRIVATE talas_runtime)
    add_test(NAME talas_prepared_midi_mapper_tests COMMAND talas_prepared_midi_mapper_tests)

    add_executable(talas_midi_mapper_large_fixture_tests
        tests/runtime/MidiMapperLargeFixtureTests.cpp
    )
    target_link_libraries(talas_midi_mapper_large_fixture_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_midi_mapper_large_fixture_tests
        COMMAND talas_midi_mapper_large_fixture_tests)
    set_tests_properties(
        talas_midi_mapper_large_fixture_tests
        PROPERTIES TIMEOUT 30)
    # Its wall-clock callback budget is checked only in this serial entry
    # (TALAS_STRICT_TIMING=1, label perf), which local_gate.sh runs after the
    # parallel suite; the entry above prints the timings without checking.
    add_test(NAME talas_midi_mapper_large_fixture_tests_strict_timing COMMAND talas_midi_mapper_large_fixture_tests)
    set_tests_properties(talas_midi_mapper_large_fixture_tests_strict_timing
        PROPERTIES ENVIRONMENT TALAS_STRICT_TIMING=1 LABELS perf RUN_SERIAL TRUE
            TIMEOUT 30)

    add_executable(talas_realtime_analysis_capture_tests
        tests/runtime/RealtimeAnalysisCaptureTests.cpp
    )
    target_link_libraries(talas_realtime_analysis_capture_tests PRIVATE talas_runtime)
    add_test(NAME talas_realtime_analysis_capture_tests COMMAND talas_realtime_analysis_capture_tests)

    add_executable(talas_realtime_output_recorder_tests
        tests/runtime/RealtimeOutputRecorderTests.cpp
    )
    target_link_libraries(talas_realtime_output_recorder_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_realtime_output_recorder_tests
        COMMAND talas_realtime_output_recorder_tests)

    add_executable(talas_realtime_audio_engine_tests
        tests/runtime/RealtimeAudioEngineTests.cpp
    )
    target_link_libraries(talas_realtime_audio_engine_tests PRIVATE talas_runtime)
    add_test(NAME talas_realtime_audio_engine_tests COMMAND talas_realtime_audio_engine_tests)

    add_executable(talas_realtime_graph_host_tests
        tests/runtime/RealtimeGraphHostTests.cpp
    )
    target_link_libraries(talas_realtime_graph_host_tests PRIVATE talas_runtime)
    add_test(NAME talas_realtime_graph_host_tests COMMAND talas_realtime_graph_host_tests)

    add_executable(talas_midi_clock_phase_follower_tests
        tests/runtime/MidiClockPhaseFollowerTests.cpp
    )
    target_link_libraries(talas_midi_clock_phase_follower_tests PRIVATE talas_runtime)
    add_test(NAME talas_midi_clock_phase_follower_tests COMMAND talas_midi_clock_phase_follower_tests)

    add_executable(talas_mtc_smpte_protocol_tests
        tests/runtime/MtcSmpteProtocolTests.cpp
    )
    target_link_libraries(talas_mtc_smpte_protocol_tests PRIVATE talas_runtime)
    add_test(NAME talas_mtc_smpte_protocol_tests COMMAND talas_mtc_smpte_protocol_tests)

    add_executable(talas_osc_protocol_tests
        tests/runtime/OscProtocolTests.cpp
    )
    target_link_libraries(talas_osc_protocol_tests PRIVATE talas_runtime)
    add_test(NAME talas_osc_protocol_tests COMMAND talas_osc_protocol_tests)

    add_executable(talas_mtc_transport_follower_tests
        tests/runtime/MtcTransportFollowerTests.cpp
    )
    target_link_libraries(talas_mtc_transport_follower_tests PRIVATE talas_runtime)
    add_test(NAME talas_mtc_transport_follower_tests COMMAND talas_mtc_transport_follower_tests)

    add_executable(talas_mtc_realtime_ingress_tests
        tests/runtime/MtcRealtimeIngressTests.cpp
    )
    target_link_libraries(talas_mtc_realtime_ingress_tests PRIVATE talas_runtime)
    add_test(NAME talas_mtc_realtime_ingress_tests COMMAND talas_mtc_realtime_ingress_tests)

    add_executable(talas_discrete4_runtime_tests
        tests/runtime/Discrete4RuntimeTests.cpp
    )
    target_link_libraries(talas_discrete4_runtime_tests PRIVATE talas_runtime)
    add_test(NAME talas_discrete4_runtime_tests COMMAND talas_discrete4_runtime_tests)

    add_executable(talas_low_pass_gate_runtime_tests
        tests/runtime/LowPassGateRuntimeTests.cpp
    )
    target_link_libraries(talas_low_pass_gate_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_low_pass_gate_runtime_tests
        COMMAND talas_low_pass_gate_runtime_tests)

    add_executable(talas_reverb_section_runtime_tests
        tests/runtime/ReverbSectionRuntimeTests.cpp
    )
    target_link_libraries(talas_reverb_section_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_reverb_section_runtime_tests
        COMMAND talas_reverb_section_runtime_tests)

    add_executable(talas_dual_morph_oscillator_runtime_tests
        tests/runtime/DualMorphOscillatorRuntimeTests.cpp
    )
    target_link_libraries(talas_dual_morph_oscillator_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_dual_morph_oscillator_runtime_tests
        COMMAND talas_dual_morph_oscillator_runtime_tests)

    add_executable(talas_logic_runtime_tests
        tests/runtime/LogicRuntimeTests.cpp
    )
    target_link_libraries(talas_logic_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_logic_runtime_tests
        COMMAND talas_logic_runtime_tests)

    add_executable(talas_annotation_runtime_tests
        tests/runtime/AnnotationRuntimeTests.cpp
    )
    target_link_libraries(talas_annotation_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_annotation_runtime_tests
        COMMAND talas_annotation_runtime_tests)

    add_executable(talas_threshold_authority_runtime_tests
        tests/runtime/ThresholdAuthorityRuntimeTests.cpp
    )
    target_link_libraries(talas_threshold_authority_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_threshold_authority_runtime_tests
        COMMAND talas_threshold_authority_runtime_tests)

    add_executable(talas_lfo_authority_runtime_tests
        tests/runtime/LfoAuthorityRuntimeTests.cpp
    )
    target_link_libraries(talas_lfo_authority_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_lfo_authority_runtime_tests
        COMMAND talas_lfo_authority_runtime_tests)

    add_executable(talas_pulse_train_authority_runtime_tests
        tests/runtime/PulseTrainAuthorityRuntimeTests.cpp
    )
    target_link_libraries(talas_pulse_train_authority_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_pulse_train_authority_runtime_tests
        COMMAND talas_pulse_train_authority_runtime_tests)

    add_executable(talas_ramp_authority_runtime_tests
        tests/runtime/RampAuthorityRuntimeTests.cpp
    )
    target_link_libraries(talas_ramp_authority_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_ramp_authority_runtime_tests
        COMMAND talas_ramp_authority_runtime_tests)

    add_executable(talas_sample_and_hold_authority_runtime_tests
        tests/runtime/SampleAndHoldAuthorityRuntimeTests.cpp
    )
    target_link_libraries(talas_sample_and_hold_authority_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_sample_and_hold_authority_runtime_tests
        COMMAND talas_sample_and_hold_authority_runtime_tests)

    add_executable(talas_quantizer_authority_runtime_tests
        tests/runtime/QuantizerAuthorityRuntimeTests.cpp
    )
    target_link_libraries(talas_quantizer_authority_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_quantizer_authority_runtime_tests
        COMMAND talas_quantizer_authority_runtime_tests)

    add_executable(talas_gate_authority_runtime_tests
        tests/runtime/GateAuthorityRuntimeTests.cpp
    )
    target_link_libraries(talas_gate_authority_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_gate_authority_runtime_tests
        COMMAND talas_gate_authority_runtime_tests)

    add_executable(talas_slew_authority_runtime_tests
        tests/runtime/SlewAuthorityRuntimeTests.cpp
    )
    target_link_libraries(talas_slew_authority_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_slew_authority_runtime_tests
        COMMAND talas_slew_authority_runtime_tests)

    add_executable(talas_clock_authority_runtime_tests
        tests/runtime/ClockAuthorityRuntimeTests.cpp
    )
    target_link_libraries(talas_clock_authority_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_clock_authority_runtime_tests
        COMMAND talas_clock_authority_runtime_tests)

    add_executable(talas_clock_divider_authority_runtime_tests
        tests/runtime/ClockDividerAuthorityRuntimeTests.cpp
    )
    target_link_libraries(
        talas_clock_divider_authority_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_clock_divider_authority_runtime_tests
        COMMAND talas_clock_divider_authority_runtime_tests)

    add_executable(talas_filter_authority_runtime_tests
        tests/runtime/FilterAuthorityRuntimeTests.cpp
    )
    target_link_libraries(
        talas_filter_authority_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_filter_authority_runtime_tests
        COMMAND talas_filter_authority_runtime_tests)

    add_executable(talas_shelving_presence_filter_authority_runtime_tests
        tests/runtime/ShelvingPresenceFilterAuthorityRuntimeTests.cpp
    )
    target_link_libraries(
        talas_shelving_presence_filter_authority_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_shelving_presence_filter_authority_runtime_tests
        COMMAND talas_shelving_presence_filter_authority_runtime_tests)

    add_executable(talas_checked_biquad_family_authority_runtime_tests
        tests/runtime/CheckedBiquadFamilyAuthorityRuntimeTests.cpp
    )
    target_link_libraries(
        talas_checked_biquad_family_authority_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_checked_biquad_family_authority_runtime_tests
        COMMAND talas_checked_biquad_family_authority_runtime_tests)

    add_executable(talas_static_fir_family_authority_runtime_tests
        tests/runtime/StaticFirFamilyAuthorityRuntimeTests.cpp
    )
    target_link_libraries(
        talas_static_fir_family_authority_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_static_fir_family_authority_runtime_tests
        COMMAND talas_static_fir_family_authority_runtime_tests)

    add_executable(talas_harmonic_resonator_authority_runtime_tests
        tests/runtime/HarmonicResonatorAuthorityRuntimeTests.cpp
    )
    target_link_libraries(
        talas_harmonic_resonator_authority_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_harmonic_resonator_authority_runtime_tests
        COMMAND talas_harmonic_resonator_authority_runtime_tests)

    add_executable(talas_dynamic_range_controller_authority_runtime_tests
        tests/runtime/DynamicRangeControllerAuthorityRuntimeTests.cpp
    )
    target_link_libraries(
        talas_dynamic_range_controller_authority_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_dynamic_range_controller_authority_runtime_tests
        COMMAND talas_dynamic_range_controller_authority_runtime_tests)

    add_executable(talas_frequency_tracker_authority_runtime_tests
        tests/runtime/FrequencyTrackerAuthorityRuntimeTests.cpp
    )
    target_link_libraries(
        talas_frequency_tracker_authority_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_frequency_tracker_authority_runtime_tests
        COMMAND talas_frequency_tracker_authority_runtime_tests)

    add_executable(talas_interpolate_authority_runtime_tests
        tests/runtime/InterpolateAuthorityRuntimeTests.cpp
    )
    target_link_libraries(
        talas_interpolate_authority_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_interpolate_authority_runtime_tests
        COMMAND talas_interpolate_authority_runtime_tests)

    add_executable(talas_scale_and_offset_authority_runtime_tests
        tests/runtime/ScaleAndOffsetAuthorityRuntimeTests.cpp
    )
    target_link_libraries(
        talas_scale_and_offset_authority_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_scale_and_offset_authority_runtime_tests
        COMMAND talas_scale_and_offset_authority_runtime_tests)

    add_executable(talas_morph_filter_runtime_tests
        tests/runtime/MorphFilterRuntimeTests.cpp
    )
    target_link_libraries(talas_morph_filter_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_morph_filter_runtime_tests
        COMMAND talas_morph_filter_runtime_tests)

    add_executable(talas_probability_chaos_generator_runtime_tests
        tests/runtime/ProbabilityChaosGeneratorRuntimeTests.cpp
    )
    target_link_libraries(talas_probability_chaos_generator_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_probability_chaos_generator_runtime_tests
        COMMAND talas_probability_chaos_generator_runtime_tests)

    add_executable(talas_quad_function_generator_runtime_tests
        tests/runtime/QuadFunctionGeneratorRuntimeTests.cpp
    )
    target_link_libraries(talas_quad_function_generator_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_quad_function_generator_runtime_tests
        COMMAND talas_quad_function_generator_runtime_tests)

    add_executable(talas_stereo_dispersive_resonator_runtime_tests
        tests/runtime/StereoDispersiveResonatorRuntimeTests.cpp
    )
    target_link_libraries(talas_stereo_dispersive_resonator_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_stereo_dispersive_resonator_runtime_tests
        COMMAND talas_stereo_dispersive_resonator_runtime_tests)

    add_executable(talas_plucked_waveguide_runtime_tests
        tests/runtime/PluckedWaveguideRuntimeTests.cpp
    )
    target_link_libraries(talas_plucked_waveguide_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_plucked_waveguide_runtime_tests
        COMMAND talas_plucked_waveguide_runtime_tests)

    add_executable(talas_dynamic_breakpoint_oscillator_runtime_tests
        tests/runtime/DynamicBreakpointOscillatorRuntimeTests.cpp
    )
    target_link_libraries(talas_dynamic_breakpoint_oscillator_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_dynamic_breakpoint_oscillator_runtime_tests
        COMMAND talas_dynamic_breakpoint_oscillator_runtime_tests)

    add_executable(talas_wave_terrain_oscillator_runtime_tests
        tests/runtime/WaveTerrainOscillatorRuntimeTests.cpp
    )
    target_link_libraries(talas_wave_terrain_oscillator_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_wave_terrain_oscillator_runtime_tests
        COMMAND talas_wave_terrain_oscillator_runtime_tests)

    add_executable(talas_scanned_mass_oscillator_runtime_tests
        tests/runtime/ScannedMassOscillatorRuntimeTests.cpp
    )
    target_link_libraries(talas_scanned_mass_oscillator_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_scanned_mass_oscillator_runtime_tests
        COMMAND talas_scanned_mass_oscillator_runtime_tests)

    add_executable(talas_diffuse_spectral_stretch_runtime_tests
        tests/runtime/DiffuseSpectralStretchRuntimeTests.cpp
    )
    target_link_libraries(talas_diffuse_spectral_stretch_runtime_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_diffuse_spectral_stretch_runtime_tests
        COMMAND talas_diffuse_spectral_stretch_runtime_tests)
    # Its wall-clock callback budget is checked only in this serial entry
    # (TALAS_STRICT_TIMING=1, label perf), which local_gate.sh runs after the
    # parallel suite; the entry above prints the timings without checking.
    add_test(NAME talas_diffuse_spectral_stretch_runtime_tests_strict_timing COMMAND talas_diffuse_spectral_stretch_runtime_tests)
    set_tests_properties(talas_diffuse_spectral_stretch_runtime_tests_strict_timing
        PROPERTIES ENVIRONMENT TALAS_STRICT_TIMING=1 LABELS perf RUN_SERIAL TRUE
            TIMEOUT 60)

    add_executable(talas_project_runtime_bridge_tests
        tests/runtime/ProjectRuntimeBridgeTests.cpp
    )
    target_link_libraries(talas_project_runtime_bridge_tests PRIVATE talas_runtime)
    add_test(NAME talas_project_runtime_bridge_tests COMMAND talas_project_runtime_bridge_tests)

    add_executable(talas_prepared_score_tests
        tests/runtime/PreparedScoreTests.cpp
    )
    target_link_libraries(talas_prepared_score_tests PRIVATE talas_runtime)
    add_test(NAME talas_prepared_score_tests COMMAND talas_prepared_score_tests)

    add_executable(talas_prepared_score_clip_source_tests
        tests/runtime/PreparedScoreClipSourceTests.cpp)
    target_link_libraries(talas_prepared_score_clip_source_tests PRIVATE talas_runtime)
    add_test(NAME talas_prepared_score_clip_source_tests COMMAND talas_prepared_score_clip_source_tests)

    add_executable(talas_envelope_extraction_tests tests/runtime/EnvelopeExtractionTests.cpp)
    target_link_libraries(talas_envelope_extraction_tests PRIVATE talas_runtime)
    add_test(NAME talas_envelope_extraction_tests COMMAND talas_envelope_extraction_tests)

    add_executable(talas_score_track_input_tests tests/runtime/ScoreTrackInputTests.cpp)
    target_link_libraries(talas_score_track_input_tests PRIVATE talas_runtime)
    add_test(NAME talas_score_track_input_tests COMMAND talas_score_track_input_tests)
    add_executable(talas_score_track_midi_tests tests/runtime/ScoreTrackMidiTests.cpp)
    target_link_libraries(talas_score_track_midi_tests PRIVATE talas_runtime)
    add_test(NAME talas_score_track_midi_tests COMMAND talas_score_track_midi_tests)

    add_executable(talas_prepared_source_callback_tests tests/runtime/PreparedSourceCallbackTests.cpp)
    target_link_libraries(talas_prepared_source_callback_tests PRIVATE talas_runtime)
    add_test(NAME talas_prepared_source_callback_tests COMMAND talas_prepared_source_callback_tests)

    add_executable(talas_prepared_timeline_wait_until_tests tests/runtime/PreparedTimelineWaitUntilTests.cpp)
    target_link_libraries(talas_prepared_timeline_wait_until_tests PRIVATE talas_runtime)
    add_test(NAME talas_prepared_timeline_wait_until_tests COMMAND talas_prepared_timeline_wait_until_tests)

    add_executable(talas_resource_snapshot_runtime_tests
        tests/runtime/ResourceSnapshotRuntimeTests.cpp
    )
    target_link_libraries(talas_resource_snapshot_runtime_tests PRIVATE talas_runtime)
    add_test(NAME talas_resource_snapshot_runtime_tests COMMAND talas_resource_snapshot_runtime_tests)

    add_executable(talas_audio_stream_tests
        tests/runtime/AudioStreamTests.cpp
    )
    target_link_libraries(talas_audio_stream_tests PRIVATE talas_runtime talas_audio)
    add_test(NAME talas_audio_stream_tests COMMAND talas_audio_stream_tests)

    add_executable(talas_wav_writer_tests
        tests/audio/WavWriterTests.cpp
    )
    target_link_libraries(talas_wav_writer_tests PRIVATE talas_audio)
    add_test(NAME talas_wav_writer_tests COMMAND talas_wav_writer_tests)

    add_executable(talas_audio_file_publication_tests
        tests/audio/AudioFilePublicationTests.cpp
    )
    target_link_libraries(talas_audio_file_publication_tests PRIVATE talas_audio)
    add_test(
        NAME talas_audio_file_publication_tests
        COMMAND talas_audio_file_publication_tests)

    add_executable(talas_project_wav_exporter_tests
        tests/audio/ProjectWavExporterTests.cpp
    )
    target_link_libraries(talas_project_wav_exporter_tests PRIVATE talas_audio_export)
    add_test(NAME talas_project_wav_exporter_tests COMMAND talas_project_wav_exporter_tests)

    add_executable(talas_timeline_recording_tests tests/audio/TimelineRecordingTests.cpp)
    target_link_libraries(talas_timeline_recording_tests PRIVATE talas_audio_export)
    add_test(NAME talas_timeline_recording_tests COMMAND talas_timeline_recording_tests)

    add_executable(talas_live_timeline_recording_tests tests/audio/LiveTimelineRecordingTests.cpp)
    target_link_libraries(talas_live_timeline_recording_tests PRIVATE talas_audio_export talas_app_support)
    add_test(NAME talas_live_timeline_recording_tests COMMAND talas_live_timeline_recording_tests)

    add_executable(talas_sound_icons_tests tests/app/SoundIconsTests.cpp)
    target_link_libraries(talas_sound_icons_tests PRIVATE talas_app_support talas_core)
    add_test(NAME talas_sound_icons_tests COMMAND talas_sound_icons_tests)

    add_executable(talas_key_equivalents_tests tests/app/TalasKeyEquivalentsTests.cpp)
    target_link_libraries(talas_key_equivalents_tests PRIVATE talas_app_support)
    add_test(NAME talas_key_equivalents_tests COMMAND talas_key_equivalents_tests)

    add_executable(talas_disk_button_tests tests/app/DiskButtonTests.cpp)
    target_link_libraries(talas_disk_button_tests PRIVATE talas_app_support)
    add_test(NAME talas_disk_button_tests COMMAND talas_disk_button_tests)

    add_executable(talas_disk_splicer_segments_tests tests/app/DiskSplicerSegmentsTests.cpp)
    target_link_libraries(talas_disk_splicer_segments_tests PRIVATE talas_app_support)
    add_test(NAME talas_disk_splicer_segments_tests COMMAND talas_disk_splicer_segments_tests)

    add_executable(talas_spectrum_synchronization_tests tests/app/SpectrumSynchronizationTests.cpp)
    target_link_libraries(talas_spectrum_synchronization_tests PRIVATE talas_app_support)
    add_test(NAME talas_spectrum_synchronization_tests COMMAND talas_spectrum_synchronization_tests)

    add_executable(talas_spectrum_table_tests tests/runtime/SpectrumTableTests.cpp)
    target_link_libraries(talas_spectrum_table_tests PRIVATE talas_app_support talas_runtime talas_audio)
    add_test(NAME talas_spectrum_table_tests COMMAND talas_spectrum_table_tests)

    add_executable(talas_quasi_harmonic_analysis_tests tests/audio/QuasiHarmonicAnalysisTests.cpp)
    target_link_libraries(talas_quasi_harmonic_analysis_tests PRIVATE talas_audio)
    add_test(NAME talas_quasi_harmonic_analysis_tests COMMAND talas_quasi_harmonic_analysis_tests)

    add_executable(talas_tape_recorder_tests tests/audio/TapeRecorderTests.cpp)
    target_link_libraries(talas_tape_recorder_tests PRIVATE talas_audio_export)
    add_test(NAME talas_tape_recorder_tests COMMAND talas_tape_recorder_tests)

    add_executable(talas_disk_cache_capture_tests
        tests/audio/DiskCacheCaptureTests.cpp
    )
    target_link_libraries(talas_disk_cache_capture_tests PRIVATE talas_audio_export)
    add_test(NAME talas_disk_cache_capture_tests COMMAND talas_disk_cache_capture_tests)

    add_executable(talas_disk_cache_large_fixture_tests
        tests/audio/DiskCacheLargeFixtureTests.cpp
    )
    target_link_libraries(
        talas_disk_cache_large_fixture_tests PRIVATE talas_audio_export)
    add_test(
        NAME talas_disk_cache_large_fixture_tests
        COMMAND talas_disk_cache_large_fixture_tests)
    set_tests_properties(
        talas_disk_cache_large_fixture_tests
        PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

    add_executable(talas_realtime_output_take_tests
        tests/audio/RealtimeOutputTakeTests.cpp
    )
    target_link_libraries(talas_realtime_output_take_tests PRIVATE talas_audio_export)
    add_test(
        NAME talas_realtime_output_take_tests
        COMMAND talas_realtime_output_take_tests)

    add_executable(talas_wav_reader_tests
        tests/audio/WavReaderTests.cpp
    )
    target_link_libraries(talas_wav_reader_tests PRIVATE talas_audio talas_pcm16_wave)
    add_test(NAME talas_wav_reader_tests COMMAND talas_wav_reader_tests)

    add_executable(talas_sample_file_formats_tests
        tests/audio/SampleFileFormatsTests.cpp
    )
    target_link_libraries(talas_sample_file_formats_tests PRIVATE talas_audio)
    add_test(NAME talas_sample_file_formats_tests COMMAND talas_sample_file_formats_tests)

    add_executable(talas_pcm16_wave_span_tests
        tests/audio/Pcm16WaveSpanTests.cpp
    )
    target_link_libraries(talas_pcm16_wave_span_tests PRIVATE talas_pcm16_wave)
    add_test(
        NAME talas_pcm16_wave_span_tests
        COMMAND talas_pcm16_wave_span_tests)

    add_executable(talas_resource_loader_tests
        tests/audio/ResourceLoaderTests.cpp
    )
    target_link_libraries(talas_resource_loader_tests PRIVATE talas_audio)
    add_test(NAME talas_resource_loader_tests COMMAND talas_resource_loader_tests)

    add_executable(talas_project_packager_tests
        tests/audio/ProjectPackagerTests.cpp
    )
    target_link_libraries(talas_project_packager_tests PRIVATE talas_audio)
    add_test(NAME talas_project_packager_tests COMMAND talas_project_packager_tests)

    add_executable(talas_sample_map_import_tests
        tests/audio/SampleMapImportTests.cpp
    )
    target_link_libraries(talas_sample_map_import_tests PRIVATE talas_audio)
    add_test(NAME talas_sample_map_import_tests COMMAND talas_sample_map_import_tests)

    add_executable(talas_sample_map_asset_workflow_tests
        tests/audio/SampleMapAssetWorkflowTests.cpp
    )
    target_link_libraries(
        talas_sample_map_asset_workflow_tests
        PRIVATE talas_audio)
    add_test(
        NAME talas_sample_map_asset_workflow_tests
        COMMAND talas_sample_map_asset_workflow_tests)

    add_executable(talas_waveform_overview_tests
        tests/audio/WaveformOverviewTests.cpp
    )
    target_link_libraries(talas_waveform_overview_tests PRIVATE talas_audio)
    add_test(NAME talas_waveform_overview_tests COMMAND talas_waveform_overview_tests)

    add_executable(talas_project_asset_change_tracker_tests
        tests/audio/ProjectAssetChangeTrackerTests.cpp
    )
    target_link_libraries(talas_project_asset_change_tracker_tests PRIVATE talas_audio)
    add_test(
        NAME talas_project_asset_change_tracker_tests
        COMMAND talas_project_asset_change_tracker_tests
    )

    add_executable(talas_spectrum_analysis_tests
        tests/audio/SpectrumAnalysisTests.cpp
    )
    target_link_libraries(talas_spectrum_analysis_tests PRIVATE talas_audio)
    add_test(NAME talas_spectrum_analysis_tests COMMAND talas_spectrum_analysis_tests)

    add_executable(talas_oscilloscope_analysis_tests
        tests/audio/OscilloscopeAnalysisTests.cpp
    )
    target_link_libraries(talas_oscilloscope_analysis_tests PRIVATE talas_audio)
    add_test(NAME talas_oscilloscope_analysis_tests COMMAND talas_oscilloscope_analysis_tests)

    add_executable(talas_analyzer_display_mapping_tests
        tests/audio/AnalyzerDisplayMappingTests.cpp
    )
    target_link_libraries(talas_analyzer_display_mapping_tests PRIVATE talas_audio)
    add_test(NAME talas_analyzer_display_mapping_tests COMMAND talas_analyzer_display_mapping_tests)

    add_executable(talas_ga_analysis_tests
        tests/audio/GaAnalysisTests.cpp
    )
    target_link_libraries(talas_ga_analysis_tests PRIVATE talas_audio)
    add_test(NAME talas_ga_analysis_tests COMMAND talas_ga_analysis_tests)

    add_executable(talas_cli_tests
        tests/cli/CliTests.cpp
    )
    target_link_libraries(talas_cli_tests PRIVATE talas_cli_lib)
    target_link_libraries(talas_cli_tests PRIVATE talas_audio)
    target_compile_definitions(talas_cli_tests PRIVATE
        TALAS_CDP_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/cdp/fixtures")
    add_test(NAME talas_cli_tests COMMAND talas_cli_tests)

    add_executable(talas_m3_examples_tests
        tests/cli/M3ExamplesTests.cpp
    )
    target_link_libraries(talas_m3_examples_tests PRIVATE talas_cli_lib)
    target_include_directories(talas_m3_examples_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/project)
    target_compile_definitions(talas_m3_examples_tests
        PRIVATE TALAS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME talas_m3_examples_tests COMMAND talas_m3_examples_tests)

    add_executable(talas_missing_asset_recovery_workflow_tests
        tests/integration/MissingAssetRecoveryWorkflowTests.cpp
    )
    target_link_libraries(talas_missing_asset_recovery_workflow_tests
        PRIVATE talas_cli_lib)
    target_compile_definitions(talas_missing_asset_recovery_workflow_tests
        PRIVATE TALAS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(
        NAME talas_missing_asset_recovery_workflow_tests
        COMMAND talas_missing_asset_recovery_workflow_tests)

    add_executable(talas_m4_state_layer_example_tests
        tests/cli/M4StateLayerExampleTests.cpp
    )
    target_link_libraries(talas_m4_state_layer_example_tests PRIVATE talas_cli_lib)
    target_compile_definitions(talas_m4_state_layer_example_tests
        PRIVATE TALAS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(
        NAME talas_m4_state_layer_example_tests
        COMMAND talas_m4_state_layer_example_tests)

    add_executable(talas_m4_sample_map_workflow_tests
        tests/integration/M4SampleMapWorkflowTests.cpp
    )
    target_link_libraries(talas_m4_sample_map_workflow_tests
        PRIVATE talas_cli_lib)
    target_compile_definitions(talas_m4_sample_map_workflow_tests
        PRIVATE TALAS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(
        NAME talas_m4_sample_map_workflow_tests
        COMMAND talas_m4_sample_map_workflow_tests)

    add_executable(talas_m4_missing_node_workflow_tests
        tests/integration/M4MissingNodeWorkflowTests.cpp
    )
    target_link_libraries(talas_m4_missing_node_workflow_tests
        PRIVATE talas_cli_lib)
    target_compile_definitions(talas_m4_missing_node_workflow_tests
        PRIVATE TALAS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(
        NAME talas_m4_missing_node_workflow_tests
        COMMAND talas_m4_missing_node_workflow_tests)

    add_executable(talas_documented_behavior_tests
        tests/runtime/DocumentedBehaviorTests.cpp
    )
    target_link_libraries(talas_documented_behavior_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_documented_behavior_tests
        COMMAND talas_documented_behavior_tests)
    # About 35 s alone, and near 120 s beside the rest of the suite.
    set_tests_properties(
        talas_documented_behavior_tests
        PROPERTIES
            LABELS "conformance"
            TIMEOUT 300)

    add_executable(talas_manual_scenario_tests
        tests/runtime/ManualScenarioTests.cpp
    )
    target_link_libraries(talas_manual_scenario_tests PRIVATE talas_runtime talas_app_support)
    add_test(
        NAME talas_manual_scenario_tests
        COMMAND talas_manual_scenario_tests)
    set_tests_properties(
        talas_manual_scenario_tests
        PROPERTIES
            LABELS "conformance"
            TIMEOUT 300)

    add_executable(talas_prototype_defaults_tests
        tests/runtime/PrototypeDefaultsTests.cpp
    )
    target_link_libraries(talas_prototype_defaults_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_prototype_defaults_tests
        COMMAND talas_prototype_defaults_tests)
    set_tests_properties(
        talas_prototype_defaults_tests
        PROPERTIES
            LABELS "conformance"
            TIMEOUT 120)

    add_executable(talas_system_wavetables_tests
        tests/runtime/SystemWavetablesTests.cpp
    )
    target_link_libraries(talas_system_wavetables_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_system_wavetables_tests
        COMMAND talas_system_wavetables_tests)

    add_executable(talas_live_parameter_parity_tests
        tests/runtime/LiveParameterParityTests.cpp
    )
    target_link_libraries(talas_live_parameter_parity_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_live_parameter_parity_tests
        COMMAND talas_live_parameter_parity_tests)
    set_tests_properties(
        talas_live_parameter_parity_tests
        PROPERTIES
            LABELS "conformance"
            TIMEOUT 600)

    add_executable(talas_plugin_host_harness_tests
        tests/integration/PluginHostHarnessTests.cpp
    )
    target_link_libraries(talas_plugin_host_harness_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_plugin_host_harness_tests
        COMMAND talas_plugin_host_harness_tests)
    set_tests_properties(
        talas_plugin_host_harness_tests
        PROPERTIES
            LABELS "integration"
            TIMEOUT 60)

    add_executable(talas_standalone_workflow_tests
        tests/integration/StandaloneWorkflowTests.cpp
    )
    target_link_libraries(talas_standalone_workflow_tests
        PRIVATE talas_cli_lib)
    add_test(
        NAME talas_standalone_workflow_tests
        COMMAND talas_standalone_workflow_tests)
    set_tests_properties(
        talas_standalone_workflow_tests
        PROPERTIES
            LABELS "integration;closeout"
            TIMEOUT 60)

    add_executable(talas_workbench_launch_tests
        tests/app/WorkbenchLaunchTests.cpp
    )
    target_link_libraries(talas_workbench_launch_tests PRIVATE talas_app_support)
    add_test(NAME talas_workbench_launch_tests COMMAND talas_workbench_launch_tests)

    add_executable(talas_workbench_ui_policy_tests
        tests/app/WorkbenchUiPolicyTests.cpp
    )
    target_link_libraries(talas_workbench_ui_policy_tests PRIVATE talas_app_support)
    add_test(NAME talas_workbench_ui_policy_tests COMMAND talas_workbench_ui_policy_tests)

    add_executable(talas_workbench_operation_coordinator_tests
        tests/app/WorkbenchOperationCoordinatorTests.cpp
    )
    target_link_libraries(
        talas_workbench_operation_coordinator_tests PRIVATE talas_app_support)
    add_test(
        NAME talas_workbench_operation_coordinator_tests
        COMMAND talas_workbench_operation_coordinator_tests)

    add_executable(talas_generator_preview_tests
        tests/app/GeneratorPreviewTests.cpp
    )
    target_link_libraries(talas_generator_preview_tests PRIVATE talas_app_support)
    add_test(
        NAME talas_generator_preview_tests
        COMMAND talas_generator_preview_tests)

    add_executable(talas_timeline_score_projection_tests
        tests/app/TimelineScoreProjectionTests.cpp
    )
    target_link_libraries(talas_timeline_score_projection_tests PRIVATE talas_app_support)
    add_test(
        NAME talas_timeline_score_projection_tests
        COMMAND talas_timeline_score_projection_tests)

    add_executable(talas_sound_browser_model_tests
        tests/app/SoundBrowserModelTests.cpp
    )
    target_link_libraries(talas_sound_browser_model_tests PRIVATE talas_app_support)
    add_test(
        NAME talas_sound_browser_model_tests
        COMMAND talas_sound_browser_model_tests)

    add_executable(talas_sound_class_icon_tests
        tests/app/SoundClassIconTests.cpp
    )
    target_link_libraries(talas_sound_class_icon_tests PRIVATE talas_app_support)
    add_test(
        NAME talas_sound_class_icon_tests
        COMMAND talas_sound_class_icon_tests)

    add_executable(talas_sound_file_window_model_tests
        tests/app/SoundFileWindowModelTests.cpp
    )
    target_link_libraries(talas_sound_file_window_model_tests PRIVATE talas_app_support)
    add_test(
        NAME talas_sound_file_window_model_tests
        COMMAND talas_sound_file_window_model_tests)

    add_executable(talas_vcs_presets_tests
        tests/app/VcsPresetsTests.cpp
    )
    target_link_libraries(talas_vcs_presets_tests PRIVATE talas_app_support talas_runtime)
    add_test(
        NAME talas_vcs_presets_tests
        COMMAND talas_vcs_presets_tests)

    add_executable(talas_timeline_info_tests
        tests/app/TimelineInfoTests.cpp
    )
    target_link_libraries(talas_timeline_info_tests PRIVATE talas_app_support talas_runtime)
    add_test(
        NAME talas_timeline_info_tests
        COMMAND talas_timeline_info_tests)

    add_executable(talas_timeline_grid_tests
        tests/app/TimelineGridTests.cpp
    )
    target_link_libraries(talas_timeline_grid_tests PRIVATE talas_app_support)
    add_test(
        NAME talas_timeline_grid_tests
        COMMAND talas_timeline_grid_tests)

    add_executable(talas_timeline_track_input_tests
        tests/app/TimelineTrackInputTests.cpp
    )
    target_link_libraries(talas_timeline_track_input_tests PRIVATE talas_app_support)
    add_test(
        NAME talas_timeline_track_input_tests
        COMMAND talas_timeline_track_input_tests)

    add_executable(talas_files_tests
        tests/app/TalasFilesTests.cpp
    )
    target_link_libraries(talas_files_tests PRIVATE talas_app_support)
    add_test(
        NAME talas_files_tests
        COMMAND talas_files_tests)

    add_executable(talas_paste_hot_model_tests
        tests/app/PasteHotModelTests.cpp
    )
    target_link_libraries(talas_paste_hot_model_tests PRIVATE talas_app_support)
    add_test(
        NAME talas_paste_hot_model_tests
        COMMAND talas_paste_hot_model_tests)

    add_executable(talas_text_search_tests
        tests/app/TextSearchTests.cpp
    )
    target_link_libraries(talas_text_search_tests PRIVATE talas_app_support)
    add_test(
        NAME talas_text_search_tests
        COMMAND talas_text_search_tests)

    add_executable(talas_dsp_status_model_tests
        tests/app/DspStatusModelTests.cpp
    )
    target_link_libraries(talas_dsp_status_model_tests PRIVATE talas_app_support)
    add_test(
        NAME talas_dsp_status_model_tests
        COMMAND talas_dsp_status_model_tests)

    add_executable(talas_partial_track_editor_model_tests
        tests/app/PartialTrackEditorModelTests.cpp
    )
    target_link_libraries(
        talas_partial_track_editor_model_tests PRIVATE talas_app_support)
    add_test(
        NAME talas_partial_track_editor_model_tests
        COMMAND talas_partial_track_editor_model_tests)

    add_executable(talas_partial_track_large_fixture_tests
        tests/app/PartialTrackLargeFixtureTests.cpp
    )
    target_link_libraries(
        talas_partial_track_large_fixture_tests
        PRIVATE talas_app_support talas_audio)
    add_test(
        NAME talas_partial_track_large_fixture_tests
        COMMAND talas_partial_track_large_fixture_tests)
    set_tests_properties(
        talas_partial_track_large_fixture_tests
        PROPERTIES RUN_SERIAL TRUE TIMEOUT 30)

    add_executable(talas_timeline_score_large_fixture_tests
        tests/app/TimelineScoreLargeFixtureTests.cpp
    )
    target_link_libraries(
        talas_timeline_score_large_fixture_tests
        PRIVATE talas_app_support talas_audio_export)
    add_test(
        NAME talas_timeline_score_large_fixture_tests
        COMMAND talas_timeline_score_large_fixture_tests)
    set_tests_properties(
        talas_timeline_score_large_fixture_tests
        PROPERTIES TIMEOUT 30)

    add_executable(talas_live_set_large_fixture_tests
        tests/app/LiveSetLargeFixtureTests.cpp
    )
    target_link_libraries(
        talas_live_set_large_fixture_tests
        PRIVATE talas_performance_runtime_dispatcher)
    add_test(
        NAME talas_live_set_large_fixture_tests
        COMMAND talas_live_set_large_fixture_tests)
    set_tests_properties(
        talas_live_set_large_fixture_tests
        PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)

    add_executable(talas_workbench_accessibility_policy_tests
        tests/app/WorkbenchAccessibilityPolicyTests.cpp
    )
    target_link_libraries(talas_workbench_accessibility_policy_tests
        PRIVATE talas_app_support)
    add_test(
        NAME talas_workbench_accessibility_policy_tests
        COMMAND talas_workbench_accessibility_policy_tests)

    add_executable(talas_automation_lane_editing_tests
        tests/app/AutomationLaneEditingTests.cpp)
    target_link_libraries(talas_automation_lane_editing_tests
        PRIVATE talas_app_support)
    add_test(
        NAME talas_automation_lane_editing_tests
        COMMAND talas_automation_lane_editing_tests)

    add_executable(talas_analysis_tools_model_tests
        tests/app/AnalysisToolsModelTests.cpp)
    target_link_libraries(talas_analysis_tools_model_tests
        PRIVATE talas_app_support)
    add_test(
        NAME talas_analysis_tools_model_tests
        COMMAND talas_analysis_tools_model_tests)

    add_executable(talas_live_grid_authoring_model_tests
        tests/app/LiveGridAuthoringModelTests.cpp)
    target_link_libraries(talas_live_grid_authoring_model_tests
        PRIVATE talas_app_support)
    add_test(
        NAME talas_live_grid_authoring_model_tests
        COMMAND talas_live_grid_authoring_model_tests)

    add_executable(talas_graph_branch_layout_tests
        tests/app/GraphBranchLayoutTests.cpp)
    target_link_libraries(talas_graph_branch_layout_tests
        PRIVATE talas_app_support)
    add_test(
        NAME talas_graph_branch_layout_tests
        COMMAND talas_graph_branch_layout_tests)

    add_executable(talas_graph_line_contour_tests
        tests/app/GraphLineContourTests.cpp)
    target_link_libraries(talas_graph_line_contour_tests
        PRIVATE talas_app_support)
    add_test(
        NAME talas_graph_line_contour_tests
        COMMAND talas_graph_line_contour_tests)

    add_executable(talas_file_locator_tests
        tests/app/FileLocatorTests.cpp)
    target_link_libraries(talas_file_locator_tests
        PRIVATE talas_app_support)
    add_test(
        NAME talas_file_locator_tests
        COMMAND talas_file_locator_tests)

    add_executable(talas_sound_information_tests
        tests/app/SoundInformationTests.cpp)
    target_link_libraries(talas_sound_information_tests
        PRIVATE talas_app_support)
    add_test(
        NAME talas_sound_information_tests
        COMMAND talas_sound_information_tests)

    add_executable(talas_sample_template_programs_tests
        tests/app/SampleTemplateProgramsTests.cpp)
    target_link_libraries(talas_sample_template_programs_tests
        PRIVATE talas_app_support)
    add_test(
        NAME talas_sample_template_programs_tests
        COMMAND talas_sample_template_programs_tests)

    add_executable(talas_parameter_field_editors_tests
        tests/app/ParameterFieldEditorsTests.cpp)
    target_link_libraries(talas_parameter_field_editors_tests
        PRIVATE talas_app_support)
    add_test(
        NAME talas_parameter_field_editors_tests
        COMMAND talas_parameter_field_editors_tests)

    add_executable(talas_sample_editor_model_tests
        tests/app/SampleEditorModelTests.cpp)
    target_link_libraries(talas_sample_editor_model_tests
        PRIVATE talas_app_support)
    add_test(
        NAME talas_sample_editor_model_tests
        COMMAND talas_sample_editor_model_tests)

    add_executable(talas_virtual_control_widget_model_tests
        tests/app/VirtualControlWidgetModelTests.cpp)
    target_link_libraries(talas_virtual_control_widget_model_tests
        PRIVATE talas_app_support)
    add_test(
        NAME talas_virtual_control_widget_model_tests
        COMMAND talas_virtual_control_widget_model_tests)

    add_executable(talas_timeline_midi_import_tests
        tests/app/TimelineMidiImportTests.cpp)
    target_link_libraries(talas_timeline_midi_import_tests
        PRIVATE talas_app_support)
    add_test(
        NAME talas_timeline_midi_import_tests
        COMMAND talas_timeline_midi_import_tests)

    add_executable(talas_class_editor_model_tests
        tests/app/ClassEditorModelTests.cpp)
    target_link_libraries(talas_class_editor_model_tests
        PRIVATE talas_app_support)
    add_test(
        NAME talas_class_editor_model_tests
        COMMAND talas_class_editor_model_tests)

    add_executable(talas_preset_event_values_model_tests
        tests/app/PresetEventValuesModelTests.cpp)
    target_link_libraries(talas_preset_event_values_model_tests
        PRIVATE talas_app_support)
    add_test(
        NAME talas_preset_event_values_model_tests
        COMMAND talas_preset_event_values_model_tests)

    add_executable(talas_on_screen_keyboard_model_tests
        tests/app/OnScreenKeyboardModelTests.cpp)
    target_link_libraries(talas_on_screen_keyboard_model_tests
        PRIVATE talas_app_support)
    add_test(
        NAME talas_on_screen_keyboard_model_tests
        COMMAND talas_on_screen_keyboard_model_tests)

    add_executable(talas_workbench_surface_state_tests
        tests/app/WorkbenchSurfaceStateTests.cpp
    )
    target_link_libraries(talas_workbench_surface_state_tests
        PRIVATE talas_app_support)
    add_test(
        NAME talas_workbench_surface_state_tests
        COMMAND talas_workbench_surface_state_tests)

    add_executable(talas_workbench_graph_viewport_index_tests
        tests/app/WorkbenchGraphViewportIndexTests.cpp)
    target_link_libraries(talas_workbench_graph_viewport_index_tests PRIVATE talas_app_support)
    add_test(
        NAME talas_workbench_graph_viewport_index_tests
        COMMAND talas_workbench_graph_viewport_index_tests)

    if(TALAS_BUILD_WORKBENCH)
        add_executable(talas_workbench_score_analysis_tests
            tests/app/WorkbenchScoreAnalysisTests.cpp)
        target_link_libraries(talas_workbench_score_analysis_tests PRIVATE talas_workbench_ui)
        add_test(NAME talas_workbench_score_analysis_tests COMMAND talas_workbench_score_analysis_tests)
        set_tests_properties(talas_workbench_score_analysis_tests PROPERTIES TIMEOUT 60)

        add_executable(talas_workbench_graph_large_fixture_tests
            tests/app/WorkbenchGraphLargeFixtureTests.cpp)
        target_link_libraries(talas_workbench_graph_large_fixture_tests
            PRIVATE talas_workbench_ui)
        add_test(
            NAME talas_workbench_graph_large_fixture_tests
            COMMAND talas_workbench_graph_large_fixture_tests)
        set_tests_properties(
            talas_workbench_graph_large_fixture_tests
            PROPERTIES TIMEOUT 60)
    endif()

    add_executable(talas_workbench_focus_coordinator_tests
        tests/app/WorkbenchFocusCoordinatorTests.cpp
        src/app/workbench/WorkbenchFocusCoordinator.cpp
    )
    target_include_directories(talas_workbench_focus_coordinator_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(talas_workbench_focus_coordinator_tests
        PRIVATE cxx_std_20)
    add_test(
        NAME talas_workbench_focus_coordinator_tests
        COMMAND talas_workbench_focus_coordinator_tests)

    add_executable(talas_prototype_browser_model_tests
        tests/app/PrototypeBrowserModelTests.cpp
    )
    target_link_libraries(talas_prototype_browser_model_tests PRIVATE talas_app_support)
    add_test(NAME talas_prototype_browser_model_tests COMMAND talas_prototype_browser_model_tests)

    add_executable(talas_descriptor_presentation_model_tests
        tests/app/DescriptorPresentationModelTests.cpp
    )
    target_link_libraries(talas_descriptor_presentation_model_tests
        PRIVATE talas_app_support)
    add_test(
        NAME talas_descriptor_presentation_model_tests
        COMMAND talas_descriptor_presentation_model_tests)

    add_executable(talas_sound_workbench_model_tests
        tests/app/SoundWorkbenchModelTests.cpp
    )
    target_link_libraries(talas_sound_workbench_model_tests PRIVATE talas_app_support)
    add_test(NAME talas_sound_workbench_model_tests COMMAND talas_sound_workbench_model_tests)

    add_executable(talas_sound_library_model_tests
        tests/app/SoundLibraryModelTests.cpp
    )
    target_link_libraries(talas_sound_library_model_tests PRIVATE talas_app_support)
    target_compile_definitions(talas_sound_library_model_tests
        PRIVATE TALAS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME talas_sound_library_model_tests COMMAND talas_sound_library_model_tests)

    add_executable(talas_sound_library_audition_model_tests
        tests/app/SoundLibraryAuditionModelTests.cpp
    )
    target_link_libraries(
        talas_sound_library_audition_model_tests PRIVATE talas_app_support talas_runtime)
    add_test(
        NAME talas_sound_library_audition_model_tests
        COMMAND talas_sound_library_audition_model_tests)

    add_executable(talas_snapshot_workbench_model_tests
        tests/app/SnapshotWorkbenchModelTests.cpp
    )
    target_link_libraries(talas_snapshot_workbench_model_tests PRIVATE talas_app_support)
    add_test(NAME talas_snapshot_workbench_model_tests COMMAND talas_snapshot_workbench_model_tests)

    add_executable(talas_performance_workbench_model_tests
        tests/app/PerformanceWorkbenchModelTests.cpp
    )
    target_link_libraries(
        talas_performance_workbench_model_tests PRIVATE talas_app_support)
    add_test(
        NAME talas_performance_workbench_model_tests
        COMMAND talas_performance_workbench_model_tests)

    add_executable(talas_analysis_workbench_model_tests
        tests/app/AnalysisWorkbenchModelTests.cpp
    )
    target_link_libraries(
        talas_analysis_workbench_model_tests PRIVATE talas_app_support)
    add_test(
        NAME talas_analysis_workbench_model_tests
        COMMAND talas_analysis_workbench_model_tests)

    add_executable(talas_analysis_audition_model_tests
        tests/app/AnalysisAuditionModelTests.cpp
    )
    target_link_libraries(
        talas_analysis_audition_model_tests PRIVATE talas_app_support)
    add_test(
        NAME talas_analysis_audition_model_tests
        COMMAND talas_analysis_audition_model_tests)

    add_executable(talas_analysis_test_input_source_tests
        tests/runtime/AnalysisTestInputSourceTests.cpp
    )
    target_link_libraries(
        talas_analysis_test_input_source_tests PRIVATE talas_runtime)
    add_test(
        NAME talas_analysis_test_input_source_tests
        COMMAND talas_analysis_test_input_source_tests)

    add_executable(talas_analysis_audition_runtime_tests
        tests/runtime/AnalysisAuditionRuntimeTests.cpp
    )
    target_link_libraries(
        talas_analysis_audition_runtime_tests PRIVATE talas_app_support talas_runtime)
    add_test(
        NAME talas_analysis_audition_runtime_tests
        COMMAND talas_analysis_audition_runtime_tests)

    add_executable(talas_analysis_diagnostics_model_tests
        tests/app/AnalysisDiagnosticsModelTests.cpp
    )
    target_link_libraries(
        talas_analysis_diagnostics_model_tests PRIVATE talas_app_support)
    add_test(
        NAME talas_analysis_diagnostics_model_tests
        COMMAND talas_analysis_diagnostics_model_tests)

    add_executable(talas_performance_runtime_dispatcher_tests
        tests/app/PerformanceRuntimeDispatcherTests.cpp
    )
    target_link_libraries(talas_performance_runtime_dispatcher_tests
        PRIVATE talas_performance_runtime_dispatcher)
    add_test(
        NAME talas_performance_runtime_dispatcher_tests
        COMMAND talas_performance_runtime_dispatcher_tests)

    add_executable(talas_live_grid_workbench_model_tests
        tests/app/LiveGridWorkbenchModelTests.cpp
    )
    target_link_libraries(talas_live_grid_workbench_model_tests
        PRIVATE talas_performance_runtime_dispatcher)
    add_test(
        NAME talas_live_grid_workbench_model_tests
        COMMAND talas_live_grid_workbench_model_tests)

    add_executable(talas_asset_recovery_workbench_model_tests
        tests/app/AssetRecoveryWorkbenchModelTests.cpp
    )
    target_link_libraries(talas_asset_recovery_workbench_model_tests PRIVATE talas_app_support)
    add_test(
        NAME talas_asset_recovery_workbench_model_tests
        COMMAND talas_asset_recovery_workbench_model_tests)

    add_executable(talas_missing_node_workbench_model_tests
        tests/app/MissingNodeWorkbenchModelTests.cpp
    )
    target_link_libraries(talas_missing_node_workbench_model_tests
        PRIVATE talas_app_support)
    add_test(
        NAME talas_missing_node_workbench_model_tests
        COMMAND talas_missing_node_workbench_model_tests)

    add_executable(talas_sample_map_interaction_tests
        tests/app/SampleMapInteractionTests.cpp
    )
    target_link_libraries(talas_sample_map_interaction_tests PRIVATE talas_app_support)
    add_test(NAME talas_sample_map_interaction_tests COMMAND talas_sample_map_interaction_tests)

    add_executable(talas_sample_map_workbench_model_tests
        tests/app/SampleMapWorkbenchModelTests.cpp
    )
    target_link_libraries(talas_sample_map_workbench_model_tests PRIVATE talas_app_support)
    add_test(NAME talas_sample_map_workbench_model_tests COMMAND talas_sample_map_workbench_model_tests)

    add_test(
        NAME talas_cli_catalog_smoke
        COMMAND talas_cli catalog --format json
    )

    add_test(
        NAME talas_generated_artifact_checks
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_generated_artifacts.py
            --cli $<TARGET_FILE:talas_cli>
    )
    set_tests_properties(
        talas_generated_artifact_checks
        PROPERTIES
            LABELS "generated"
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    add_test(
        NAME talas_line_ending_policy_check
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_line_endings.py
    )
    set_tests_properties(
        talas_line_ending_policy_check
        PROPERTIES
            LABELS "repository"
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Realtime test executables that exercise several threads; local_gate.sh
    # --tsan runs them under ThreadSanitizer. A sharded one's ctest entries
    # are listed in <executable>_shards.
    foreach(threaded_test IN ITEMS
            talas_realtime_render_session_tests
            talas_realtime_graph_host_tests
            talas_realtime_control_ingress_tests
            talas_realtime_live_set_ingress_tests
            talas_realtime_analysis_capture_tests
            talas_mtc_realtime_ingress_tests
            talas_audio_stream_tests
            talas_project_runtime_bridge_tests
            talas_analysis_audition_runtime_tests
            talas_prepared_score_clip_source_tests
            talas_virtual_control_midi_tests
            talas_resource_snapshot_runtime_tests
            talas_disk_cache_capture_tests)
        if(DEFINED ${threaded_test}_shards)
            set_property(TEST ${${threaded_test}_shards}
                APPEND PROPERTY LABELS "threaded")
        else()
            set_property(TEST ${threaded_test} APPEND PROPERTY LABELS "threaded")
        endif()
    endforeach()

    add_test(
        NAME talas_cli_oscillator_example
        COMMAND talas_cli render
            --project ${CMAKE_CURRENT_SOURCE_DIR}/examples/oscillator.talas.json
            --duration 1
            --sample-rate 8
    )

    add_test(
        NAME talas_cli_graph_oscillator_example
        COMMAND talas_cli graph
            --project ${CMAKE_CURRENT_SOURCE_DIR}/examples/oscillator.talas.json
            --format dot
    )

    add_test(
        NAME talas_cli_init_oscillator_smoke
        COMMAND ${CMAKE_COMMAND}
            -D TALAS_CLI_EXE=$<TARGET_FILE:talas_cli>
            -D TALAS_INIT_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/talas_cli_init_oscillator_smoke.talas.json
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli/init_smoke.cmake
    )

    add_test(
        NAME talas_cli_validate_oscillator_example
        COMMAND talas_cli validate
            --project ${CMAKE_CURRENT_SOURCE_DIR}/examples/oscillator.talas.json
    )

    add_test(
        NAME talas_cli_sample_table_example
        COMMAND talas_cli render
            --project ${CMAKE_CURRENT_SOURCE_DIR}/examples/sample_text_table.talas.json
            --frames 4
            --sample-rate 4
    )

    add_test(
        NAME talas_cli_validate_sample_table_example
        COMMAND talas_cli validate
            --project ${CMAKE_CURRENT_SOURCE_DIR}/examples/sample_text_table.talas.json
    )

    add_test(
        NAME talas_cli_validate_reusable_feedback_help
        COMMAND talas_cli validate
            --project ${CMAKE_CURRENT_SOURCE_DIR}/examples/reusable_feedback_help.talas.json
    )

    add_test(
        NAME talas_cli_render_reusable_feedback_help
        COMMAND talas_cli render
            --project ${CMAKE_CURRENT_SOURCE_DIR}/examples/reusable_feedback_help.talas.json
            --frames 256
            --sample-rate 8000
            --format json
    )
    set_tests_properties(
        talas_cli_oscillator_example
        talas_cli_graph_oscillator_example
        talas_cli_init_oscillator_smoke
        talas_cli_validate_oscillator_example
        talas_cli_sample_table_example
        talas_cli_validate_sample_table_example
        talas_cli_validate_reusable_feedback_help
        talas_cli_render_reusable_feedback_help
        PROPERTIES LABELS "example-smoke")
    if(TALAS_BUILD_FUZZERS)
        if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            message(FATAL_ERROR
                "TALAS_BUILD_FUZZERS requires Clang sanitizer support")
        endif()

        if(TALAS_FUZZ_ENGINE STREQUAL "LIBFUZZER")
            set(TALAS_FUZZ_COMPILE_OPTIONS
                -fsanitize=fuzzer-no-link,address,undefined
                -fno-sanitize-recover=undefined
                -fno-omit-frame-pointer)
            set(TALAS_FUZZ_LINK_OPTIONS
                -fsanitize=fuzzer,address,undefined
                -fno-omit-frame-pointer)
            set(TALAS_FUZZ_DRIVER_SOURCE)
            set(TALAS_FUZZ_RUNTIME_OPTIONS -timeout=5)
        elseif(TALAS_FUZZ_ENGINE STREQUAL "REPLAY")
            set(TALAS_FUZZ_COMPILE_OPTIONS
                -fsanitize=address,undefined
                -fno-sanitize-recover=undefined
                -fno-omit-frame-pointer)
            set(TALAS_FUZZ_LINK_OPTIONS
                -fsanitize=address,undefined
                -fno-omit-frame-pointer)
            set(TALAS_FUZZ_DRIVER_SOURCE tests/fuzz/FuzzReplayMain.cpp)
            set(TALAS_FUZZ_RUNTIME_OPTIONS)
        else()
            message(FATAL_ERROR
                "TALAS_FUZZ_ENGINE must be REPLAY or LIBFUZZER")
        endif()

        target_compile_options(talas_core PRIVATE ${TALAS_FUZZ_COMPILE_OPTIONS})
        target_compile_options(talas_generative PRIVATE ${TALAS_FUZZ_COMPILE_OPTIONS})
        target_compile_options(talas_project PRIVATE ${TALAS_FUZZ_COMPILE_OPTIONS})

        add_executable(talas_project_json_fuzzer
            tests/fuzz/ProjectJsonFuzzer.cpp
            ${TALAS_FUZZ_DRIVER_SOURCE})
        target_link_libraries(talas_project_json_fuzzer PRIVATE talas_project)
        target_compile_options(
            talas_project_json_fuzzer PRIVATE ${TALAS_FUZZ_COMPILE_OPTIONS})
        target_link_options(
            talas_project_json_fuzzer PRIVATE ${TALAS_FUZZ_LINK_OPTIONS})

        add_executable(talas_expression_fuzzer
            tests/fuzz/ExpressionFuzzer.cpp
            ${TALAS_FUZZ_DRIVER_SOURCE})
        target_link_libraries(talas_expression_fuzzer PRIVATE talas_core)
        target_compile_options(
            talas_expression_fuzzer PRIVATE ${TALAS_FUZZ_COMPILE_OPTIONS})
        target_link_options(
            talas_expression_fuzzer PRIVATE ${TALAS_FUZZ_LINK_OPTIONS})

        add_executable(talas_context_free_generator_fuzzer
            tests/fuzz/ContextFreeGeneratorFuzzer.cpp
            ${TALAS_FUZZ_DRIVER_SOURCE})
        target_link_libraries(
            talas_context_free_generator_fuzzer PRIVATE talas_generative)
        target_compile_options(
            talas_context_free_generator_fuzzer PRIVATE ${TALAS_FUZZ_COMPILE_OPTIONS})
        target_link_options(
            talas_context_free_generator_fuzzer PRIVATE ${TALAS_FUZZ_LINK_OPTIONS})

        add_test(
            NAME talas_project_json_fuzz_smoke
            COMMAND talas_project_json_fuzzer
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/project-json
                -runs=2048
                -seed=12648430
                -max_len=65536
                ${TALAS_FUZZ_RUNTIME_OPTIONS})
        add_test(
            NAME talas_expression_fuzz_smoke
            COMMAND talas_expression_fuzzer
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/expression
                -runs=4096
                -seed=195936478
                -max_len=4096
                ${TALAS_FUZZ_RUNTIME_OPTIONS})
        add_test(
            NAME talas_context_free_generator_fuzz_smoke
            COMMAND talas_context_free_generator_fuzzer
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/context-free-generator
                -runs=4096
                -seed=1128679217
                -max_len=65536
                ${TALAS_FUZZ_RUNTIME_OPTIONS})
        set_tests_properties(
            talas_project_json_fuzz_smoke
            talas_expression_fuzz_smoke
            talas_context_free_generator_fuzz_smoke
            PROPERTIES
                LABELS "fuzz-smoke"
                TIMEOUT 60)
    endif()
    if(TALAS_BUILD_BENCHMARKS)
        if(NOT CMAKE_GENERATOR STREQUAL "Unix Makefiles")
            message(FATAL_ERROR
                "Core performance benchmark manifest requires Unix Makefiles")
        endif()
        if(NOT Python3_Interpreter_FOUND)
            message(FATAL_ERROR
                "Core performance benchmark manifest requires Python 3")
        endif()
        execute_process(
            COMMAND git rev-parse --verify HEAD
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE TALAS_BENCHMARK_COMMIT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_VARIABLE TALAS_BENCHMARK_GIT_ERROR
            RESULT_VARIABLE TALAS_BENCHMARK_GIT_RESULT)
        string(LENGTH "${TALAS_BENCHMARK_COMMIT}" TALAS_BENCHMARK_COMMIT_LENGTH)
        if(NOT TALAS_BENCHMARK_GIT_RESULT EQUAL 0
            OR NOT TALAS_BENCHMARK_COMMIT_LENGTH EQUAL 40
            OR NOT TALAS_BENCHMARK_COMMIT MATCHES "^[0-9a-f]+$")
            message(FATAL_ERROR
                "Cannot bind benchmark build to Git HEAD: ${TALAS_BENCHMARK_GIT_ERROR}")
        endif()
        execute_process(
            COMMAND git diff --quiet HEAD --
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            RESULT_VARIABLE TALAS_BENCHMARK_DIRTY_RESULT)
        if(NOT TALAS_BENCHMARK_DIRTY_RESULT EQUAL 0)
            message(FATAL_ERROR
                "Performance benchmark configure requires clean tracked sources")
        endif()
        execute_process(
            COMMAND git rev-parse --git-path index
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE TALAS_BENCHMARK_GIT_INDEX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE TALAS_BENCHMARK_GIT_INDEX_RESULT)
        if(NOT TALAS_BENCHMARK_GIT_INDEX_RESULT EQUAL 0)
            message(FATAL_ERROR "Cannot resolve Git index for benchmark provenance")
        endif()
        if(NOT IS_ABSOLUTE "${TALAS_BENCHMARK_GIT_INDEX}")
            set(TALAS_BENCHMARK_GIT_INDEX
                "${CMAKE_CURRENT_SOURCE_DIR}/${TALAS_BENCHMARK_GIT_INDEX}")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/.git/HEAD"
            "${TALAS_BENCHMARK_GIT_INDEX}")
        set(TALAS_BENCHMARK_BUILD_CONTRACT
            "generator=${CMAKE_GENERATOR}|generatorPlatform=${CMAKE_GENERATOR_PLATFORM}|generatorToolset=${CMAKE_GENERATOR_TOOLSET}|cmake=${CMAKE_VERSION}|buildType=${CMAKE_BUILD_TYPE}")
        get_cmake_property(TALAS_BENCHMARK_CACHE_VARIABLES CACHE_VARIABLES)
        list(SORT TALAS_BENCHMARK_CACHE_VARIABLES)
        foreach(TALAS_BENCHMARK_CACHE_NAME IN LISTS TALAS_BENCHMARK_CACHE_VARIABLES)
            get_property(TALAS_BENCHMARK_CACHE_VALUE
                CACHE "${TALAS_BENCHMARK_CACHE_NAME}" PROPERTY VALUE)
            if((TALAS_BENCHMARK_CACHE_NAME MATCHES
                    "^CMAKE_PROJECT_.*_INCLUDE(_BEFORE)?$"
                OR TALAS_BENCHMARK_CACHE_NAME STREQUAL
                    "CMAKE_PROJECT_TOP_LEVEL_INCLUDES"
                OR TALAS_BENCHMARK_CACHE_NAME MATCHES
                    "^CMAKE_USER_MAKE_RULES_OVERRIDE(_.*)?$")
                AND NOT TALAS_BENCHMARK_CACHE_VALUE STREQUAL "")
                message(FATAL_ERROR
                    "Performance benchmark rejects CMake project/rule injection: ${TALAS_BENCHMARK_CACHE_NAME}")
            endif()
            if(TALAS_BENCHMARK_CACHE_NAME MATCHES "^TALAS_"
                OR TALAS_BENCHMARK_CACHE_NAME STREQUAL "BUILD_SHARED_LIBS"
                OR TALAS_BENCHMARK_CACHE_NAME MATCHES
                    "^CMAKE_(C_|CXX_|AR$|RANLIB$|LINKER($|_)|EXE_LINKER_|SHARED_LINKER_|MODULE_LINKER_|STATIC_LINKER_|INTERPROCEDURAL_OPTIMIZATION($|_)|OSX_|TOOLCHAIN_FILE$|POSITION_INDEPENDENT_CODE$|UNITY_BUILD($|_)|MSVC_RUNTIME_LIBRARY$|BUILD_RPATH|INSTALL_RPATH|SKIP_RPATH|BUILD_WITH_INSTALL_RPATH|PREFIX_PATH$|FIND_ROOT_PATH|FRAMEWORK_PATH$|LIBRARY_PATH$|INCLUDE_PATH$|SYSROOT($|_)|APPLE_SILICON_PROCESSOR$|CROSSCOMPILING_EMULATOR$|PROJECT_TOP_LEVEL_INCLUDES$|PROJECT_INCLUDE($|_)|USER_MAKE_RULES_OVERRIDE($|_)|POLICY_DEFAULT_)")
                get_property(TALAS_BENCHMARK_CACHE_TYPE
                    CACHE "${TALAS_BENCHMARK_CACHE_NAME}" PROPERTY TYPE)
                string(LENGTH "${TALAS_BENCHMARK_CACHE_NAME}"
                    TALAS_BENCHMARK_CACHE_NAME_LENGTH)
                string(LENGTH "${TALAS_BENCHMARK_CACHE_TYPE}"
                    TALAS_BENCHMARK_CACHE_TYPE_LENGTH)
                string(LENGTH "${TALAS_BENCHMARK_CACHE_VALUE}"
                    TALAS_BENCHMARK_CACHE_VALUE_LENGTH)
                string(APPEND TALAS_BENCHMARK_BUILD_CONTRACT
                    "|${TALAS_BENCHMARK_CACHE_NAME_LENGTH}:${TALAS_BENCHMARK_CACHE_NAME}"
                    "|${TALAS_BENCHMARK_CACHE_TYPE_LENGTH}:${TALAS_BENCHMARK_CACHE_TYPE}"
                    "|${TALAS_BENCHMARK_CACHE_VALUE_LENGTH}:${TALAS_BENCHMARK_CACHE_VALUE}")
            endif()
        endforeach()
        string(SHA256 TALAS_BENCHMARK_CONFIGURATION_FINGERPRINT
            "${TALAS_BENCHMARK_BUILD_CONTRACT}")
        message(STATUS
            "Core benchmark configuration fingerprint: ${TALAS_BENCHMARK_CONFIGURATION_FINGERPRINT}")
        set(TALAS_BENCHMARK_BUILD_MANIFEST
            "${CMAKE_CURRENT_BINARY_DIR}/talas-performance-build-manifest.json")
        set(TALAS_BENCHMARK_BUILD_FINGERPRINT_FILE
            "${CMAKE_CURRENT_BINARY_DIR}/talas-performance-build-fingerprint.txt")
        add_custom_target(talas_core_performance_benchmark_provenance
            COMMAND ${CMAKE_COMMAND}
                "-DTALAS_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DTALAS_EXPECTED_COMMIT=${TALAS_BENCHMARK_COMMIT}"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_performance_benchmark_source.cmake
            VERBATIM)
        add_executable(talas_core_performance_benchmarks
            benchmarks/CorePerformanceBenchmarks.cpp)
        add_dependencies(talas_core_performance_benchmarks
            talas_core_performance_benchmark_provenance)
        target_link_libraries(talas_core_performance_benchmarks PRIVATE talas_runtime)
        set_property(TARGET talas_core_performance_benchmarks APPEND PROPERTY
            LINK_DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/capture_compiler_invocation.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/write_performance_build_manifest.py")
        target_compile_definitions(talas_core_performance_benchmarks PRIVATE
            TALAS_BENCHMARK_COMMIT="${TALAS_BENCHMARK_COMMIT}"
            TALAS_BENCHMARK_PRESET_ID="${TALAS_BENCHMARK_PRESET_ID}"
            TALAS_BENCHMARK_FINGERPRINT_FILE="${TALAS_BENCHMARK_BUILD_FINGERPRINT_FILE}"
            TALAS_BENCHMARK_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
            TALAS_BENCHMARK_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
            TALAS_BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
        add_custom_command(TARGET talas_core_performance_benchmarks POST_BUILD
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/write_performance_build_manifest.py
                --compile-commands
                    ${CMAKE_CURRENT_BINARY_DIR}/compile_commands.json
                --invocation-log-dir
                    ${TALAS_BENCHMARK_COMPILER_INVOCATION_LOG_DIR}
                --link-command
                    ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/talas_core_performance_benchmarks.dir/link.txt
                --source-root ${CMAKE_CURRENT_SOURCE_DIR}
                --build-root ${CMAKE_CURRENT_BINARY_DIR}
                --generator "${CMAKE_GENERATOR}"
                --configuration-fingerprint
                    ${TALAS_BENCHMARK_CONFIGURATION_FINGERPRINT}
                --manifest ${TALAS_BENCHMARK_BUILD_MANIFEST}
                --fingerprint ${TALAS_BENCHMARK_BUILD_FINGERPRINT_FILE}
            BYPRODUCTS
                ${TALAS_BENCHMARK_BUILD_MANIFEST}
                ${TALAS_BENCHMARK_BUILD_FINGERPRINT_FILE}
            VERBATIM)
        add_test(
            NAME talas_core_performance_benchmark_smoke
            COMMAND talas_core_performance_benchmarks
                --warmups=1
                --samples=4
                --callback-samples=32)
        set_tests_properties(
            talas_core_performance_benchmark_smoke
            PROPERTIES LABELS "benchmark-smoke" TIMEOUT 60)
    endif()
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME talas_coverage_tool_tests
            COMMAND
                ${Python3_EXECUTABLE}
                -m unittest discover
                -s ${CMAKE_CURRENT_SOURCE_DIR}/tests/coverage
                -p "test_*.py"
        )
        add_test(
            NAME talas_governance_tool_tests
            COMMAND
                ${Python3_EXECUTABLE}
                -m unittest discover
                -s ${CMAKE_CURRENT_SOURCE_DIR}/tests/governance
                -p "test_*.py"
        )
        add_test(
            NAME talas_release_configuration_tests
            COMMAND
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/release/test_release_configuration.py
        )
    endif()
endif()

set(CPACK_PACKAGE_NAME "Talas")
set(CPACK_PACKAGE_VENDOR "Talas")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Realtime modular sound-design Workbench and CLI")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/packages")
set(CPACK_RESOURCE_FILE_LICENSE
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
if(APPLE AND TALAS_BUILD_WORKBENCH AND TALAS_ENABLE_DMG_PACKAGE)
    set(CPACK_GENERATOR "ZIP;DragNDrop")
elseif(APPLE OR WIN32)
    set(CPACK_GENERATOR "ZIP")
else()
    set(CPACK_GENERATOR "TGZ")
endif()
include(CPack)

# Sanitizers slow every test down, ThreadSanitizer by 5-15x even at the
# core-tsan preset's -O1, so a sanitizer build multiplies each test's own
# TIMEOUT by this factor: a 30 s bound set for Release becomes 120 s there.
if(CMAKE_CXX_FLAGS MATCHES "-fsanitize=[^ ]*thread")
    set(talas_default_test_timeout_scale 4)
elseif(CMAKE_CXX_FLAGS MATCHES "-fsanitize=[^ ]*address")
    set(talas_default_test_timeout_scale 2)
else()
    set(talas_default_test_timeout_scale 1)
endif()
set(TALAS_TEST_TIMEOUT_SCALE "${talas_default_test_timeout_scale}" CACHE STRING
    "Factor for each test's own TIMEOUT (default 4 under TSan, 2 under ASan, else 1)")
if(NOT TALAS_TEST_TIMEOUT_SCALE MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR
        "TALAS_TEST_TIMEOUT_SCALE must be a positive integer, not '${TALAS_TEST_TIMEOUT_SCALE}'")
endif()

# Hang protection: every registered test without its own TIMEOUT gets a
# generous bound, so one deadlock cannot stall ctest indefinitely. Tests
# with their own TIMEOUT get it scaled by TALAS_TEST_TIMEOUT_SCALE.
if(TALAS_BUILD_TESTS)
    get_directory_property(talas_registered_tests TESTS)
    foreach(talas_registered_test IN LISTS talas_registered_tests)
        get_test_property(${talas_registered_test} TIMEOUT talas_registered_timeout)
        if(NOT talas_registered_timeout)
            set_tests_properties(${talas_registered_test} PROPERTIES TIMEOUT 1800)
        elseif(NOT TALAS_TEST_TIMEOUT_SCALE EQUAL 1)
            math(EXPR talas_scaled_timeout
                "${talas_registered_timeout} * ${TALAS_TEST_TIMEOUT_SCALE}")
            set_tests_properties(${talas_registered_test}
                PROPERTIES TIMEOUT ${talas_scaled_timeout})
        endif()
    endforeach()
endif()
