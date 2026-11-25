#include "talas/app/SoundLibraryModel.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectJson.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace
{
class TemporaryDirectory
{
public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path()
                / ("talas-sound-library-model-"
                   + std::to_string(
                       std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count())))
    {
        std::filesystem::create_directories(path_ / "examples/resources");
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::string manifestText()
{
    std::ifstream input(
        std::filesystem::path{TALAS_SOURCE_DIR}
            / "examples/sound-library-manifest.json",
        std::ios::binary);
    require(input.is_open(), "sound library manifest should be readable");
    return {std::istreambuf_iterator<char>{input}, {}};
}

std::string fileText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    require(input.is_open(), "test project should be readable");
    return {std::istreambuf_iterator<char>{input}, {}};
}

void writeText(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(output.is_open(), "test project should be writable");
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(output.good(), "test project bytes should be written");
}

std::string replacedOnce(
    std::string value,
    std::string_view from,
    std::string_view to)
{
    const auto position = value.find(from);
    require(position != std::string::npos, "mutation source should exist");
    value.replace(position, from.size(), to);
    return value;
}

void testCheckedManifestLoadsExactOriginalCorpus()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto result = talas::app::loadSoundLibraryManifest(
        std::filesystem::path{TALAS_SOURCE_DIR}, registry);
    require(result.ok(), "checked sound library manifest should load: "
            + result.diagnostic);
    require(result.manifest->schemaVersion == 1
                && result.manifest->entries.size() == 202,
        "checked manifest should bind the two hundred two generated original "
        "examples, the twenty-four demos among them");
    const auto spectrumModifier = std::ranges::find(
        result.manifest->entries,
        "bounded-spectrum-modifier",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        spectrumModifier != result.manifest->entries.end()
            && spectrumModifier->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && spectrumModifier->helpAnchor
                == "#bounded-spectrum-modifier"
            && talas::app::soundLibraryHasAction(
                spectrumModifier->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                spectrumModifier->actions,
                talas::app::SoundLibraryAction::Audition),
        "bounded Spectrum Modifier example should expose exact C27 open, audition, render, and local-help authority");
    const auto cloudBank = std::ranges::find(
        result.manifest->entries,
        "cloud-bank-checked-spectrum",
        &talas::app::SoundLibraryManifestEntry::id);
    require(cloudBank != result.manifest->entries.end()
            && cloudBank->projectPath
                == "examples/c111_cloud_bank_checked_spectrum.talas.json"
            && cloudBank->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && cloudBank->helpAnchor == "#cloud-bank-checked-spectrum"
            && talas::app::soundLibraryHasAction(
                cloudBank->actions,
                talas::app::SoundLibraryAction::Audition),
        "C111 Cloud Bank example should expose exact open, audition, render, and local-help authority");
    const auto cloudBankElement = std::ranges::find(
        result.manifest->entries,
        "cloud-bank-element-range-cascade-delay",
        &talas::app::SoundLibraryManifestEntry::id);
    require(cloudBankElement != result.manifest->entries.end()
            && cloudBankElement->projectPath
                == "examples/c112_cloud_bank_element_range_cascade_delay.talas.json"
            && cloudBankElement->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && cloudBankElement->helpAnchor
                == "#cloud-bank-element-range-cascade-delay"
            && talas::app::soundLibraryHasAction(
                cloudBankElement->actions,
                talas::app::SoundLibraryAction::Audition),
        "C112 Cloud Bank Element example should expose exact open, audition, render, and local-help authority");
    const auto grainCloud = std::ranges::find(
        result.manifest->entries,
        "grain-cloud-dynamic-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(grainCloud != result.manifest->entries.end()
            && grainCloud->projectPath
                == "examples/c113_grain_cloud_dynamic_controls.talas.json"
            && grainCloud->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && grainCloud->helpAnchor
                == "#grain-cloud-dynamic-controls"
            && talas::app::soundLibraryHasAction(
                grainCloud->actions,
                talas::app::SoundLibraryAction::Audition),
        "C113 Grain Cloud example should expose exact open, audition, render, and local-help authority");
    const auto chopper = std::ranges::find(
        result.manifest->entries,
        "chopper-dynamic-durations",
        &talas::app::SoundLibraryManifestEntry::id);
    require(chopper != result.manifest->entries.end()
            && chopper->projectPath
                == "examples/c114_chopper_dynamic_durations.talas.json"
            && chopper->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && chopper->helpAnchor
                == "#chopper-dynamic-durations"
            && talas::app::soundLibraryHasAction(
                chopper->actions,
                talas::app::SoundLibraryAction::Audition),
        "C114 Chopper example should expose exact open, audition, render, and local-help authority");
    const auto functionGenerator = std::ranges::find(
        result.manifest->entries,
        "function-generator-trigger-duration",
        &talas::app::SoundLibraryManifestEntry::id);
    require(functionGenerator != result.manifest->entries.end()
            && functionGenerator->projectPath
                == "examples/c115_function_generator_trigger_duration.talas.json"
            && functionGenerator->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && functionGenerator->helpAnchor
                == "#function-generator-trigger-duration"
            && talas::app::soundLibraryHasAction(
                functionGenerator->actions,
                talas::app::SoundLibraryAction::Audition),
        "C115 Function Generator example should expose exact open, audition, render, and local-help authority");
    const auto multislope = std::ranges::find(
        result.manifest->entries,
        "multislope-connected-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(multislope != result.manifest->entries.end()
            && multislope->projectPath
                == "examples/c116_multislope_connected_controls.talas.json"
            && multislope->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && multislope->helpAnchor
                == "#multislope-connected-controls"
            && talas::app::soundLibraryHasAction(
                multislope->actions,
                talas::app::SoundLibraryAction::Audition),
        "C116 Multislope example should expose exact open, audition, render, and local-help authority");
    const auto multisegment = std::ranges::find(
        result.manifest->entries,
        "multisegment-loop-connected-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(multisegment != result.manifest->entries.end()
            && multisegment->projectPath
                == "examples/c117_multisegment_loop_connected_controls.talas.json"
            && multisegment->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && multisegment->helpAnchor
                == "#multisegment-loop-connected-controls"
            && talas::app::soundLibraryHasAction(
                multisegment->actions,
                talas::app::SoundLibraryAction::Audition),
        "C117 Multisegment example should expose exact open, audition, render, and local-help authority");
    const auto noise = std::ranges::find(
        result.manifest->entries,
        "noise-modes-connected-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(noise != result.manifest->entries.end()
            && noise->projectPath
                == "examples/c118_noise_modes_connected_controls.talas.json"
            && noise->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && noise->helpAnchor
                == "#noise-modes-connected-controls"
            && talas::app::soundLibraryHasAction(
                noise->actions,
                talas::app::SoundLibraryAction::Audition),
        "C118 Noise example should expose exact open, audition, render, and local-help authority");
    const auto absoluteValue = std::ranges::find(
        result.manifest->entries,
        "absolute-value-connected-magnitude",
        &talas::app::SoundLibraryManifestEntry::id);
    require(absoluteValue != result.manifest->entries.end()
            && absoluteValue->projectPath
                == "examples/c119_absolute_value_connected_magnitude.talas.json"
            && absoluteValue->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && absoluteValue->helpAnchor
                == "#absolute-value-connected-magnitude"
            && talas::app::soundLibraryHasAction(
                absoluteValue->actions,
                talas::app::SoundLibraryAction::Audition),
        "C119 Absolute Value example should expose exact open, audition, render, and local-help authority");
    const auto arcTan = std::ranges::find(
        result.manifest->entries,
        "arctan-connected-curve",
        &talas::app::SoundLibraryManifestEntry::id);
    require(arcTan != result.manifest->entries.end()
            && arcTan->projectPath
                == "examples/c120_arctan_connected_curve.talas.json"
            && arcTan->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && arcTan->helpAnchor
                == "#arctan-connected-curve"
            && talas::app::soundLibraryHasAction(
                arcTan->actions,
                talas::app::SoundLibraryAction::Audition),
        "C120 ArcTan example should expose exact open, audition, render, and local-help authority");
    const auto equality = std::ranges::find(
        result.manifest->entries,
        "equality-connected-comparison",
        &talas::app::SoundLibraryManifestEntry::id);
    require(equality != result.manifest->entries.end()
            && equality->projectPath
                == "examples/c121_equality_connected_comparison.talas.json"
            && equality->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && equality->helpAnchor
                == "#equality-connected-comparison"
            && talas::app::soundLibraryHasAction(
                equality->actions,
                talas::app::SoundLibraryAction::Audition),
        "C121 Equality example should expose exact open, audition, render, and local-help authority");
    const auto setRange = std::ranges::find(
        result.manifest->entries,
        "set-range-connected-affine-mapping",
        &talas::app::SoundLibraryManifestEntry::id);
    require(setRange != result.manifest->entries.end()
            && setRange->projectPath
                == "examples/c122_set_range_connected_affine_mapping.talas.json"
            && setRange->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && setRange->helpAnchor
                == "#set-range-connected-affine-mapping"
            && talas::app::soundLibraryHasAction(
                setRange->actions,
                talas::app::SoundLibraryAction::Audition),
        "C122 Set Range example should expose exact open, audition, render, and local-help authority");
    const auto product = std::ranges::find(
        result.manifest->entries,
        "product-connected-finite-multiplication",
        &talas::app::SoundLibraryManifestEntry::id);
    require(product != result.manifest->entries.end()
            && product->projectPath
                == "examples/c123_product_connected_finite_multiplication.talas.json"
            && product->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && product->helpAnchor
                == "#product-connected-finite-multiplication"
            && talas::app::soundLibraryHasAction(
                product->actions,
                talas::app::SoundLibraryAction::Audition),
        "C123 Product example should expose exact open, audition, render, and local-help authority");
    const auto difference = std::ranges::find(
        result.manifest->entries,
        "difference-connected-finite-subtraction",
        &talas::app::SoundLibraryManifestEntry::id);
    require(difference != result.manifest->entries.end()
            && difference->projectPath
                == "examples/c124_difference_connected_finite_subtraction.talas.json"
            && difference->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && difference->helpAnchor
                == "#difference-connected-finite-subtraction"
            && talas::app::soundLibraryHasAction(
                difference->actions,
                talas::app::SoundLibraryAction::Audition),
        "C124 Difference example should expose exact open, audition, render, and local-help authority");
    const auto sqrtMagnitude = std::ranges::find(
        result.manifest->entries,
        "sqrt-magnitude-connected-saturated-norm",
        &talas::app::SoundLibraryManifestEntry::id);
    require(sqrtMagnitude != result.manifest->entries.end()
            && sqrtMagnitude->projectPath
                == "examples/c125_sqrt_magnitude_connected_saturated_norm.talas.json"
            && sqrtMagnitude->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && sqrtMagnitude->helpAnchor
                == "#sqrt-magnitude-connected-saturated-norm"
            && talas::app::soundLibraryHasAction(
                sqrtMagnitude->actions,
                talas::app::SoundLibraryAction::Audition),
        "C125 Sqrt Magnitude example should expose exact open, audition, render, and local-help authority");
    const auto vca = std::ranges::find(
        result.manifest->entries,
        "vca-connected-finite-gain",
        &talas::app::SoundLibraryManifestEntry::id);
    require(vca != result.manifest->entries.end()
            && vca->projectPath
                == "examples/c126_vca_connected_finite_gain.talas.json"
            && vca->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && vca->helpAnchor
                == "#vca-connected-finite-gain"
            && talas::app::soundLibraryHasAction(
                vca->actions,
                talas::app::SoundLibraryAction::Audition),
        "C126 VCA example should expose exact open, audition, render, and local-help authority");
    const auto constant = std::ranges::find(
        result.manifest->entries,
        "constant-finite-event-value-broadcast",
        &talas::app::SoundLibraryManifestEntry::id);
    require(constant != result.manifest->entries.end()
            && constant->projectPath
                == "examples/c127_constant_finite_event_value_broadcast.talas.json"
            && constant->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && constant->helpAnchor
                == "#constant-finite-event-value-broadcast"
            && talas::app::soundLibraryHasAction(
                constant->actions,
                talas::app::SoundLibraryAction::Audition),
        "C127 Constant example should expose exact open, audition, render, and local-help authority");
    const auto add = std::ranges::find(
        result.manifest->entries,
        "add-finite-event-value-sum",
        &talas::app::SoundLibraryManifestEntry::id);
    require(add != result.manifest->entries.end()
            && add->projectPath
                == "examples/c128_add_finite_event_value_sum.talas.json"
            && add->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && add->helpAnchor
                == "#add-finite-event-value-sum"
            && talas::app::soundLibraryHasAction(
                add->actions,
                talas::app::SoundLibraryAction::Audition),
        "C128 Add example should expose exact open, audition, render, and local-help authority");
    const auto mixer = std::ranges::find(
        result.manifest->entries,
        "mixer-checked-finite-weighted-sum",
        &talas::app::SoundLibraryManifestEntry::id);
    require(mixer != result.manifest->entries.end()
            && mixer->projectPath
                == "examples/c129_mixer_checked_finite_weighted_sum.talas.json"
            && mixer->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && mixer->helpAnchor
                == "#mixer-checked-finite-weighted-sum"
            && talas::app::soundLibraryHasAction(
                mixer->actions,
                talas::app::SoundLibraryAction::Audition),
        "C129 Mixer example should expose exact open, audition, render, and local-help authority");
    const auto level = std::ranges::find(
        result.manifest->entries,
        "level-checked-finite-stereo-gain",
        &talas::app::SoundLibraryManifestEntry::id);
    require(level != result.manifest->entries.end()
            && level->projectPath
                == "examples/c130_level_checked_finite_stereo_gain.talas.json"
            && level->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && level->helpAnchor
                == "#level-checked-finite-stereo-gain"
            && talas::app::soundLibraryHasAction(
                level->actions,
                talas::app::SoundLibraryAction::Audition),
        "C130 Level example should expose exact open, audition, render, and local-help authority");
    const auto channelCrosser = std::ranges::find(
        result.manifest->entries,
        "channel-crosser-checked-finite-matrix",
        &talas::app::SoundLibraryManifestEntry::id);
    require(channelCrosser != result.manifest->entries.end()
            && channelCrosser->projectPath
                == "examples/c131_channel_crosser_checked_finite_matrix.talas.json"
            && channelCrosser->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && channelCrosser->helpAnchor
                == "#channel-crosser-checked-finite-matrix"
            && talas::app::soundLibraryHasAction(
                channelCrosser->actions,
                talas::app::SoundLibraryAction::Audition),
        "C131 Channel Crosser example should expose exact open, audition, render, and local-help authority");
    const auto channelJoin = std::ranges::find(
        result.manifest->entries,
        "channel-join-checked-finite-dual-lane",
        &talas::app::SoundLibraryManifestEntry::id);
    require(channelJoin != result.manifest->entries.end()
            && channelJoin->projectPath
                == "examples/c132_channel_join_checked_finite_dual_lane.talas.json"
            && channelJoin->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && channelJoin->helpAnchor
                == "#channel-join-checked-finite-dual-lane"
            && talas::app::soundLibraryHasAction(
                channelJoin->actions,
                talas::app::SoundLibraryAction::Audition),
        "C132 Channel Join example should expose exact open, audition, render, and local-help authority");
    const auto channeller = std::ranges::find(
        result.manifest->entries,
        "channeller-checked-finite-live-selectors",
        &talas::app::SoundLibraryManifestEntry::id);
    require(channeller != result.manifest->entries.end()
            && channeller->projectPath
                == "examples/c133_channeller_checked_finite_live_selectors.talas.json"
            && channeller->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && channeller->helpAnchor
                == "#channeller-checked-finite-live-selectors"
            && talas::app::soundLibraryHasAction(
                channeller->actions,
                talas::app::SoundLibraryAction::Audition),
        "C133 Channeller example should expose exact open, audition, render, and local-help authority");
    const auto pan = std::ranges::find(
        result.manifest->entries,
        "pan-checked-finite-live-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(pan != result.manifest->entries.end()
            && pan->projectPath
                == "examples/c134_pan_checked_finite_live_controls.talas.json"
            && pan->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && pan->helpAnchor
                == "#pan-checked-finite-live-controls"
            && talas::app::soundLibraryHasAction(
                pan->actions,
                talas::app::SoundLibraryAction::Audition),
        "C134 Pan example should expose exact open, audition, render, and local-help authority");
    const auto crossfade = std::ranges::find(
        result.manifest->entries,
        "crossfade-checked-finite-live-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(crossfade != result.manifest->entries.end()
            && crossfade->projectPath
                == "examples/c135_crossfade_checked_finite_live_controls.talas.json"
            && crossfade->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && crossfade->helpAnchor
                == "#crossfade-checked-finite-live-controls"
            && talas::app::soundLibraryHasAction(
                crossfade->actions,
                talas::app::SoundLibraryAction::Audition),
        "C135 Crossfade example should expose exact open, audition, render, and local-help authority");
    const auto stereoMix2 = std::ranges::find(
        result.manifest->entries,
        "stereo-mix-2-checked-live-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(stereoMix2 != result.manifest->entries.end()
            && stereoMix2->projectPath
                == "examples/c136_stereo_mix2_checked_live_controls.talas.json"
            && stereoMix2->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && stereoMix2->helpAnchor
                == "#stereo-mix-2-checked-live-controls"
            && talas::app::soundLibraryHasAction(
                stereoMix2->actions,
                talas::app::SoundLibraryAction::Audition),
        "C136 Stereo Mix 2 example should expose exact open, audition, render, and local-help authority");
    const auto stereoMix4 = std::ranges::find(
        result.manifest->entries,
        "stereo-mix-4-checked-live-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(stereoMix4 != result.manifest->entries.end()
            && stereoMix4->projectPath
                == "examples/c137_stereo_mix4_checked_live_controls.talas.json"
            && stereoMix4->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && stereoMix4->helpAnchor
                == "#stereo-mix-4-checked-live-controls"
            && talas::app::soundLibraryHasAction(
                stereoMix4->actions,
                talas::app::SoundLibraryAction::Audition),
        "C137 Stereo Mix 4 example should expose exact open, audition, render, and local-help authority");
    const auto audioOutput = std::ranges::find(
        result.manifest->entries,
        "audio-output-exact-stereo-terminal",
        &talas::app::SoundLibraryManifestEntry::id);
    require(audioOutput != result.manifest->entries.end()
            && audioOutput->projectPath
                == "examples/c138_audio_output_exact_stereo_terminal.talas.json"
            && audioOutput->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && audioOutput->helpAnchor
                == "#audio-output-exact-stereo-terminal"
            && talas::app::soundLibraryHasAction(
                audioOutput->actions,
                talas::app::SoundLibraryAction::Audition),
        "C138 Audio Output example should expose exact open, audition, render, and local-help authority");
    const auto averagingFilter = std::ranges::find(
        result.manifest->entries,
        "averaging-low-pass-filter-dynamic-window",
        &talas::app::SoundLibraryManifestEntry::id);
    require(averagingFilter != result.manifest->entries.end()
            && averagingFilter->projectPath
                == "examples/c139_averaging_low_pass_filter_dynamic_window.talas.json"
            && averagingFilter->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && averagingFilter->helpAnchor
                == "#averaging-low-pass-filter-dynamic-window"
            && talas::app::soundLibraryHasAction(
                averagingFilter->actions,
                talas::app::SoundLibraryAction::Audition),
        "C139 Averaging Low Pass Filter example should expose exact open, audition, render, and local-help authority");
    const auto feedbackDelay = std::ranges::find(
        result.manifest->entries,
        "delay-with-feedback-dynamic-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(feedbackDelay != result.manifest->entries.end()
            && feedbackDelay->projectPath
                == "examples/c140_delay_with_feedback_dynamic_controls.talas.json"
            && feedbackDelay->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && feedbackDelay->helpAnchor
                == "#delay-with-feedback-dynamic-controls"
            && talas::app::soundLibraryHasAction(
                feedbackDelay->actions,
                talas::app::SoundLibraryAction::Audition),
        "C140 Delay With Feedback example should expose exact open, audition, render, and local-help authority");
    const auto setDuration = std::ranges::find(
        result.manifest->entries,
        "set-duration-bounded-window",
        &talas::app::SoundLibraryManifestEntry::id);
    require(setDuration != result.manifest->entries.end()
            && setDuration->projectPath
                == "examples/c141_set_duration_bounded_window.talas.json"
            && setDuration->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && setDuration->helpAnchor
                == "#set-duration-bounded-window"
            && talas::app::soundLibraryHasAction(
                setDuration->actions,
                talas::app::SoundLibraryAction::Audition),
        "C141 Set Duration example should expose exact open, audition, render, and local-help authority");
    const auto warpedTimeIndexScheduled = std::ranges::find(
        result.manifest->entries,
        "warped-time-index-scheduled-morph",
        &talas::app::SoundLibraryManifestEntry::id);
    require(warpedTimeIndexScheduled != result.manifest->entries.end()
            && warpedTimeIndexScheduled->projectPath
                == "examples/c142_warped_time_index_scheduled_morph.talas.json"
            && warpedTimeIndexScheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && warpedTimeIndexScheduled->helpAnchor
                == "#warped-time-index-scheduled-morph"
            && talas::app::soundLibraryHasAction(
                warpedTimeIndexScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C142 Warped Time Index example should expose exact open, audition, render, and local-help authority");
    const auto amplitudeFollowerScheduled = std::ranges::find(
        result.manifest->entries,
        "amplitude-follower-scheduled-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(amplitudeFollowerScheduled != result.manifest->entries.end()
            && amplitudeFollowerScheduled->projectPath
                == "examples/c143_amplitude_follower_scheduled_controls.talas.json"
            && amplitudeFollowerScheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && amplitudeFollowerScheduled->helpAnchor
                == "#amplitude-follower-scheduled-controls"
            && talas::app::soundLibraryHasAction(
                amplitudeFollowerScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C143 Amplitude Follower example should expose exact open, audition, render, and local-help authority");
    const auto peakDetectorScheduled = std::ranges::find(
        result.manifest->entries,
        "peak-detector-scheduled-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(peakDetectorScheduled != result.manifest->entries.end()
            && peakDetectorScheduled->projectPath
                == "examples/c144_peak_detector_scheduled_controls.talas.json"
            && peakDetectorScheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && peakDetectorScheduled->helpAnchor
                == "#peak-detector-scheduled-controls"
            && talas::app::soundLibraryHasAction(
                peakDetectorScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C144 Peak Detector example should expose exact open, audition, render, and local-help authority");
    const auto rmsSquaredScheduled = std::ranges::find(
        result.manifest->entries,
        "rms-squared-scheduled-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(rmsSquaredScheduled != result.manifest->entries.end()
            && rmsSquaredScheduled->projectPath
                == "examples/c145_rms_squared_scheduled_controls.talas.json"
            && rmsSquaredScheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && rmsSquaredScheduled->helpAnchor
                == "#rms-squared-scheduled-controls"
            && talas::app::soundLibraryHasAction(
                rmsSquaredScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C145 RMS Squared example should expose exact open, audition, render, and local-help authority");
    const auto runningMaxScheduled = std::ranges::find(
        result.manifest->entries,
        "running-max-scheduled-reset",
        &talas::app::SoundLibraryManifestEntry::id);
    require(runningMaxScheduled != result.manifest->entries.end()
            && runningMaxScheduled->projectPath
                == "examples/c146_running_max_scheduled_reset.talas.json"
            && runningMaxScheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && runningMaxScheduled->helpAnchor
                == "#running-max-scheduled-reset"
            && talas::app::soundLibraryHasAction(
                runningMaxScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C146 Running Max example should expose exact open, audition, render, and local-help authority");
    const auto runningMinScheduled = std::ranges::find(
        result.manifest->entries,
        "running-min-scheduled-reset-baseline",
        &talas::app::SoundLibraryManifestEntry::id);
    require(runningMinScheduled != result.manifest->entries.end()
            && runningMinScheduled->projectPath
                == "examples/c147_running_min_scheduled_reset_baseline.talas.json"
            && runningMinScheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && runningMinScheduled->helpAnchor
                == "#running-min-scheduled-reset-baseline"
            && talas::app::soundLibraryHasAction(
                runningMinScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C147 Running Min example should expose exact open, audition, render, and local-help authority");
    const auto triggeredSampleAndHoldScheduled = std::ranges::find(
        result.manifest->entries,
        "triggered-sample-and-hold-scheduled-edges",
        &talas::app::SoundLibraryManifestEntry::id);
    require(triggeredSampleAndHoldScheduled != result.manifest->entries.end()
            && triggeredSampleAndHoldScheduled->projectPath
                == "examples/c148_triggered_sample_and_hold_scheduled_edges.talas.json"
            && triggeredSampleAndHoldScheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && triggeredSampleAndHoldScheduled->helpAnchor
                == "#triggered-sample-and-hold-scheduled-edges"
            && talas::app::soundLibraryHasAction(
                triggeredSampleAndHoldScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C148 Triggered Sample And Hold example should expose exact open, audition, render, and local-help authority");
    const auto triggeredTableReadScheduled = std::ranges::find(
        result.manifest->entries,
        "triggered-table-read-paired-schedules",
        &talas::app::SoundLibraryManifestEntry::id);
    require(triggeredTableReadScheduled != result.manifest->entries.end()
            && triggeredTableReadScheduled->projectPath
                == "examples/c149_triggered_table_read_paired_schedules.talas.json"
            && triggeredTableReadScheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && triggeredTableReadScheduled->helpAnchor
                == "#triggered-table-read-paired-schedules"
            && talas::app::soundLibraryHasAction(
                triggeredTableReadScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C149 Triggered Table Read example should expose exact open, audition, render, and local-help authority");
    const auto functionGeneratorScheduled = std::ranges::find(
        result.manifest->entries,
        "function-generator-scheduled-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(functionGeneratorScheduled != result.manifest->entries.end()
            && functionGeneratorScheduled->projectPath
                == "examples/c150_function_generator_scheduled_controls.talas.json"
            && functionGeneratorScheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && functionGeneratorScheduled->helpAnchor
                == "#function-generator-scheduled-controls"
            && talas::app::soundLibraryHasAction(
                functionGeneratorScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C150 Function Generator example should expose exact open, audition, render, and local-help authority");
    const auto multislopeScheduled = std::ranges::find(
        result.manifest->entries,
        "multislope-scheduled-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(multislopeScheduled != result.manifest->entries.end()
            && multislopeScheduled->projectPath
                == "examples/c151_multislope_scheduled_controls.talas.json"
            && multislopeScheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && multislopeScheduled->helpAnchor
                == "#multislope-scheduled-controls"
            && talas::app::soundLibraryHasAction(
                multislopeScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C151 Multislope example should expose exact open, audition, render, and local-help authority");
    const auto multisegmentScheduled = std::ranges::find(
        result.manifest->entries,
        "multisegment-scheduled-loop-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(multisegmentScheduled != result.manifest->entries.end()
            && multisegmentScheduled->projectPath
                == "examples/c152_multisegment_scheduled_loop_controls.talas.json"
            && multisegmentScheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && multisegmentScheduled->helpAnchor
                == "#multisegment-scheduled-loop-controls"
            && talas::app::soundLibraryHasAction(
                multisegmentScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C152 Multisegment example should expose exact open, audition, render, and local-help authority");
    const auto graphicalScheduled = std::ranges::find(
        result.manifest->entries,
        "graphical-envelope-scheduled-rate-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(graphicalScheduled != result.manifest->entries.end()
            && graphicalScheduled->projectPath
                == "examples/c153_graphical_envelope_scheduled_rate_controls.talas.json"
            && graphicalScheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && graphicalScheduled->helpAnchor
                == "#graphical-envelope-scheduled-rate-controls"
            && talas::app::soundLibraryHasAction(
                graphicalScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C153 Graphical Envelope example should expose exact open, audition, render, and local-help authority");
    const auto adsrScheduled = std::ranges::find(
        result.manifest->entries,
        "adsr-scheduled-typed-gate-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(adsrScheduled != result.manifest->entries.end()
            && adsrScheduled->projectPath
                == "examples/c154_adsr_scheduled_typed_gate_controls.talas.json"
            && adsrScheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && adsrScheduled->helpAnchor
                == "#adsr-scheduled-typed-gate-controls"
            && talas::app::soundLibraryHasAction(
                adsrScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C154 ADSR example should expose exact open, audition, render, and local-help authority");
    const auto oscillatorScheduled = std::ranges::find(
        result.manifest->entries,
        "oscillator-scheduled-phase-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(oscillatorScheduled != result.manifest->entries.end()
            && oscillatorScheduled->projectPath
                == "examples/c155_oscillator_scheduled_phase_controls.talas.json"
            && oscillatorScheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && oscillatorScheduled->helpAnchor
                == "#oscillator-scheduled-phase-controls"
            && talas::app::soundLibraryHasAction(
                oscillatorScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C155 Oscillator example should expose exact open, audition, render, and local-help authority");
    const auto chopperScheduled = std::ranges::find(
        result.manifest->entries,
        "chopper-scheduled-cycle-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(chopperScheduled != result.manifest->entries.end()
            && chopperScheduled->projectPath
                == "examples/c156_chopper_scheduled_cycle_controls.talas.json"
            && chopperScheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && chopperScheduled->helpAnchor
                == "#chopper-scheduled-cycle-controls"
            && talas::app::soundLibraryHasAction(
                chopperScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C156 Chopper example should expose exact open, audition, render, and local-help authority");
    const auto analogSequencerScheduled = std::ranges::find(
        result.manifest->entries,
        "analog-sequencer-scheduled-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(analogSequencerScheduled != result.manifest->entries.end()
            && analogSequencerScheduled->projectPath
                == "examples/c157_analog_sequencer_scheduled_controls.talas.json"
            && analogSequencerScheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && analogSequencerScheduled->helpAnchor
                == "#analog-sequencer-scheduled-controls"
            && talas::app::soundLibraryHasAction(
                analogSequencerScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C157 Analog Sequencer example should expose exact open, audition, render, and local-help authority");
    const auto arScheduled = std::ranges::find(
        result.manifest->entries,
        "ar-scheduled-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(arScheduled != result.manifest->entries.end()
            && arScheduled->projectPath
                == "examples/c158_ar_scheduled_controls.talas.json"
            && arScheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && arScheduled->helpAnchor
                == "#ar-scheduled-controls"
            && talas::app::soundLibraryHasAction(
                arScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C163 AR example should expose exact open, audition, render, and local-help authority");
    const auto analysisFilterScheduled = std::ranges::find(
        result.manifest->entries,
        "analysis-filter-scheduled-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(analysisFilterScheduled != result.manifest->entries.end()
            && analysisFilterScheduled->projectPath
                == "examples/c159_analysis_filter_scheduled_controls.talas.json"
            && analysisFilterScheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && analysisFilterScheduled->helpAnchor
                == "#analysis-filter-scheduled-controls"
            && talas::app::soundLibraryHasAction(
                analysisFilterScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C164 Analysis Filter example should expose exact open, audition, render, and local-help authority");
    const auto phaseShiftBy90Scheduled = std::ranges::find(
        result.manifest->entries,
        "phase-shift-by-90-scheduled-frequency",
        &talas::app::SoundLibraryManifestEntry::id);
    require(phaseShiftBy90Scheduled != result.manifest->entries.end()
            && phaseShiftBy90Scheduled->projectPath
                == "examples/c160_phase_shift_by_90_scheduled_frequency.talas.json"
            && phaseShiftBy90Scheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && phaseShiftBy90Scheduled->helpAnchor
                == "#phase-shift-by-90-scheduled-frequency"
            && talas::app::soundLibraryHasAction(
                phaseShiftBy90Scheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C165 Phase Shift By 90 example should expose exact open, audition, render, and local-help authority");
    const auto singleSideBandRmScheduled = std::ranges::find(
        result.manifest->entries,
        "single-side-band-rm-scheduled-quadrature",
        &talas::app::SoundLibraryManifestEntry::id);
    require(singleSideBandRmScheduled != result.manifest->entries.end()
            && singleSideBandRmScheduled->projectPath
                == "examples/c161_single_side_band_rm_scheduled_quadrature.talas.json"
            && singleSideBandRmScheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && singleSideBandRmScheduled->helpAnchor
                == "#single-side-band-rm-scheduled-quadrature"
            && talas::app::soundLibraryHasAction(
                singleSideBandRmScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C166 Single Side Band RM example should expose exact open, audition, render, and local-help authority");
    const auto sosOscillatorsStaticSineBank = std::ranges::find(
        result.manifest->entries,
        "sos-oscillators-static-spectrum-sine-bank",
        &talas::app::SoundLibraryManifestEntry::id);
    require(sosOscillatorsStaticSineBank != result.manifest->entries.end()
            && sosOscillatorsStaticSineBank->projectPath
                == "examples/c162_sos_oscillators_static_spectrum_sine_bank.talas.json"
            && sosOscillatorsStaticSineBank->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && sosOscillatorsStaticSineBank->helpAnchor
                == "#sos-oscillators-static-spectrum-sine-bank"
            && talas::app::soundLibraryHasAction(
                sosOscillatorsStaticSineBank->actions,
                talas::app::SoundLibraryAction::Audition),
        "C167 SOS Oscillators example should expose exact open, audition, render, and local-help authority");
    const auto lowPassGateScheduled = std::ranges::find(
        result.manifest->entries,
        "low-pass-gate-scheduled-control",
        &talas::app::SoundLibraryManifestEntry::id);
    require(lowPassGateScheduled != result.manifest->entries.end()
            && lowPassGateScheduled->projectPath
                == "examples/c163_low_pass_gate_scheduled_control.talas.json"
            && lowPassGateScheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && lowPassGateScheduled->helpAnchor
                == "#low-pass-gate-scheduled-control"
            && talas::app::soundLibraryHasAction(
                lowPassGateScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C168 Low-Pass Gate example should expose exact open, audition, render, and local-help authority");
    const auto morphFilterScheduled = std::ranges::find(
        result.manifest->entries,
        "morph-filter-scheduled-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(morphFilterScheduled != result.manifest->entries.end()
            && morphFilterScheduled->projectPath
                == "examples/c164_morph_filter_scheduled_controls.talas.json"
            && morphFilterScheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && morphFilterScheduled->helpAnchor
                == "#morph-filter-scheduled-controls"
            && talas::app::soundLibraryHasAction(
                morphFilterScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C169 Morph Filter example should expose exact open, audition, render, and local-help authority");
    const auto quadFunctionGeneratorScheduled = std::ranges::find(
        result.manifest->entries,
        "quad-function-generator-scheduled-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(quadFunctionGeneratorScheduled != result.manifest->entries.end()
            && quadFunctionGeneratorScheduled->projectPath
                == "examples/c165_quad_function_generator_scheduled_controls.talas.json"
            && quadFunctionGeneratorScheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && quadFunctionGeneratorScheduled->helpAnchor
                == "#quad-function-generator-scheduled-controls"
            && talas::app::soundLibraryHasAction(
                quadFunctionGeneratorScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C170 Quad Function Generator example should expose exact open, audition, render, and local-help authority");
    const auto probabilityChaosGeneratorScheduled = std::ranges::find(
        result.manifest->entries,
        "probability-chaos-generator-scheduled-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(probabilityChaosGeneratorScheduled != result.manifest->entries.end()
            && probabilityChaosGeneratorScheduled->projectPath
                == "examples/c166_probability_chaos_generator_scheduled_controls.talas.json"
            && probabilityChaosGeneratorScheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && probabilityChaosGeneratorScheduled->helpAnchor
                == "#probability-chaos-generator-scheduled-controls"
            && talas::app::soundLibraryHasAction(
                probabilityChaosGeneratorScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C171 Probability Chaos Generator example should expose exact open, audition, render, and local-help authority");
    const auto stereoDispersiveResonatorScheduled = std::ranges::find(
        result.manifest->entries,
        "stereo-dispersive-resonator-scheduled-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(stereoDispersiveResonatorScheduled != result.manifest->entries.end()
            && stereoDispersiveResonatorScheduled->projectPath
                == "examples/c167_stereo_dispersive_resonator_scheduled_controls.talas.json"
            && stereoDispersiveResonatorScheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && stereoDispersiveResonatorScheduled->helpAnchor
                == "#stereo-dispersive-resonator-scheduled-controls"
            && talas::app::soundLibraryHasAction(
                stereoDispersiveResonatorScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C172 Stereo Dispersive Resonator example should expose exact open, audition, render, and local-help authority");
    const auto pluckedWaveguideScheduled = std::ranges::find(
        result.manifest->entries,
        "plucked-waveguide-scheduled-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(pluckedWaveguideScheduled != result.manifest->entries.end()
            && pluckedWaveguideScheduled->projectPath
                == "examples/c168_plucked_waveguide_scheduled_controls.talas.json"
            && pluckedWaveguideScheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && pluckedWaveguideScheduled->helpAnchor
                == "#plucked-waveguide-scheduled-controls"
            && talas::app::soundLibraryHasAction(
                pluckedWaveguideScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C173 Plucked Waveguide example should expose exact open, audition, render, and local-help authority");
    const auto dynamicBreakpointScheduled = std::ranges::find(
        result.manifest->entries,
        "dynamic-breakpoint-oscillator-scheduled-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(dynamicBreakpointScheduled != result.manifest->entries.end()
            && dynamicBreakpointScheduled->projectPath
                == "examples/c169_dynamic_breakpoint_oscillator_scheduled_controls.talas.json"
            && dynamicBreakpointScheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && dynamicBreakpointScheduled->helpAnchor
                == "#dynamic-breakpoint-oscillator-scheduled-controls"
            && talas::app::soundLibraryHasAction(
                dynamicBreakpointScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C174 Dynamic Breakpoint example should expose exact open, audition, render, and local-help authority");
    const auto waveTerrainScheduled = std::ranges::find(
        result.manifest->entries,
        "wave-terrain-oscillator-scheduled-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(waveTerrainScheduled != result.manifest->entries.end()
            && waveTerrainScheduled->projectPath
                == "examples/c170_wave_terrain_oscillator_scheduled_controls.talas.json"
            && waveTerrainScheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && waveTerrainScheduled->helpAnchor
                == "#wave-terrain-oscillator-scheduled-controls"
            && talas::app::soundLibraryHasAction(
                waveTerrainScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C175 Wave Terrain example should expose exact open, audition, render, and local-help authority");
    const auto scannedMassScheduled = std::ranges::find(
        result.manifest->entries,
        "scanned-mass-oscillator-scheduled-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(scannedMassScheduled != result.manifest->entries.end()
            && scannedMassScheduled->projectPath
                == "examples/c171_scanned_mass_oscillator_scheduled_controls.talas.json"
            && scannedMassScheduled->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && scannedMassScheduled->helpAnchor
                == "#scanned-mass-oscillator-scheduled-controls"
            && talas::app::soundLibraryHasAction(
                scannedMassScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C176 Scanned Mass example should expose exact open, audition, render, and local-help authority");
    const auto diffuseSpectralStretchScheduled = std::ranges::find(
        result.manifest->entries, "diffuse-spectral-stretch-scheduled-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        diffuseSpectralStretchScheduled != result.manifest->entries.end()
            && diffuseSpectralStretchScheduled->projectPath
                   == "examples/"
                      "c172_diffuse_spectral_stretch_scheduled_controls.talas.json"
            && diffuseSpectralStretchScheduled->expectedOutcome
                   == talas::app::SoundLibraryExpectedOutcome::Renderable
            && diffuseSpectralStretchScheduled->helpAnchor
                   == "#diffuse-spectral-stretch-scheduled-controls"
            && talas::app::soundLibraryHasAction(
                diffuseSpectralStretchScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C177 Diffuse Spectral Stretch example should expose exact open, "
        "audition, render, and local-help authority");
    const auto dualMorphOscillatorScheduled = std::ranges::find(
        result.manifest->entries, "dual-morph-oscillator-scheduled-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        dualMorphOscillatorScheduled != result.manifest->entries.end()
            && dualMorphOscillatorScheduled->projectPath
                   == "examples/"
                      "c173_dual_morph_oscillator_scheduled_controls.talas.json"
            && dualMorphOscillatorScheduled->expectedOutcome
                   == talas::app::SoundLibraryExpectedOutcome::Renderable
            && dualMorphOscillatorScheduled->helpAnchor
                   == "#dual-morph-oscillator-scheduled-controls"
            && talas::app::soundLibraryHasAction(
                dualMorphOscillatorScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C178 Dual Morph Oscillator example should expose exact open, "
        "audition, render, and local-help authority");
    const auto logicScheduled = std::ranges::find(
        result.manifest->entries, "logic-scheduled-fallbacks",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        logicScheduled != result.manifest->entries.end()
            && logicScheduled->projectPath
                   == "examples/c174_logic_scheduled_fallbacks.talas.json"
            && logicScheduled->expectedOutcome
                   == talas::app::SoundLibraryExpectedOutcome::Renderable
            && logicScheduled->helpAnchor
                   == "#logic-scheduled-fallbacks"
            && talas::app::soundLibraryHasAction(
                logicScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C179 Logic example should expose exact open, audition, render, "
        "and local-help authority");
    const auto annotationPassThrough = std::ranges::find(
        result.manifest->entries, "annotation-passthrough",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        annotationPassThrough != result.manifest->entries.end()
            && annotationPassThrough->projectPath
                   == "examples/c175_annotation_passthrough.talas.json"
            && annotationPassThrough->expectedOutcome
                   == talas::app::SoundLibraryExpectedOutcome::Renderable
            && annotationPassThrough->helpAnchor
                   == "#annotation-pass-through"
            && talas::app::soundLibraryHasAction(
                annotationPassThrough->actions,
                talas::app::SoundLibraryAction::Audition),
        "C180 Annotation example should expose exact open, audition, render, "
        "and local-help authority");
    const auto filterScheduled = std::ranges::find(
        result.manifest->entries, "filter-scheduled-controls",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        filterScheduled != result.manifest->entries.end()
            && filterScheduled->projectPath
                   == "examples/c191_filter_scheduled_controls.talas.json"
            && filterScheduled->expectedOutcome
                   == talas::app::SoundLibraryExpectedOutcome::Renderable
            && filterScheduled->helpAnchor
                   == "#filter-scheduled-controls"
            && talas::app::soundLibraryHasAction(
                filterScheduled->actions,
                talas::app::SoundLibraryAction::Audition),
        "C191 Filter example should expose exact open, audition, render, "
        "and local-help authority");
    const auto syntheticSpectrumFromSounds = std::ranges::find(
        result.manifest->entries, "bounded-synthetic-spectrum-from-sounds",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        syntheticSpectrumFromSounds != result.manifest->entries.end()
            && syntheticSpectrumFromSounds->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && syntheticSpectrumFromSounds->helpAnchor
                == "#bounded-synthetic-spectrum-from-sounds"
            && talas::app::soundLibraryHasAction(
                syntheticSpectrumFromSounds->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                syntheticSpectrumFromSounds->actions,
                talas::app::SoundLibraryAction::Audition),
        "C29 example should expose exact open, audition, render, and local-help authority");
    const auto warpedTimeIndex = std::ranges::find(
        result.manifest->entries,
        "bounded-warped-time-index",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        warpedTimeIndex != result.manifest->entries.end()
            && warpedTimeIndex->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && warpedTimeIndex->helpAnchor
                == "#bounded-warped-time-index"
            && talas::app::soundLibraryHasAction(
                warpedTimeIndex->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                warpedTimeIndex->actions,
                talas::app::SoundLibraryAction::Audition),
        "C30 example should expose exact open, audition, render, and local-help authority");
    const auto spectrumOnDisk = std::ranges::find(
        result.manifest->entries,
        "bounded-spectrum-on-disk",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        spectrumOnDisk != result.manifest->entries.end()
            && spectrumOnDisk->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && spectrumOnDisk->helpAnchor
                == "#bounded-spectrum-on-disk"
            && talas::app::soundLibraryHasAction(
                spectrumOnDisk->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                spectrumOnDisk->actions,
                talas::app::SoundLibraryAction::Audition),
        "C31 example should expose exact open, audition, render, and local-help authority");
    const auto gaOscillators = std::ranges::find(
        result.manifest->entries,
        "bounded-ga-oscillators",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        gaOscillators != result.manifest->entries.end()
            && gaOscillators->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && gaOscillators->helpAnchor
                == "#bounded-ga-oscillators"
            && talas::app::soundLibraryHasAction(
                gaOscillators->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                gaOscillators->actions,
                talas::app::SoundLibraryAction::Audition),
        "C32 example should expose exact open, audition, render, and local-help authority");
    const auto genericSource = std::ranges::find(
        result.manifest->entries,
        "bounded-generic-source-ram-sample",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        genericSource != result.manifest->entries.end()
            && genericSource->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && genericSource->helpAnchor
                == "#bounded-generic-source-ram-sample"
            && talas::app::soundLibraryHasAction(
                genericSource->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                genericSource->actions,
                talas::app::SoundLibraryAction::Audition),
        "C33 example should expose exact open, audition, render, and local-help authority");
    const auto multifileDiskPlayer = std::ranges::find(
        result.manifest->entries,
        "bounded-multifile-disk-player",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        multifileDiskPlayer != result.manifest->entries.end()
            && multifileDiskPlayer->projectPath
                == "examples/c37_bounded_multifile_disk_player.talas.json"
            && multifileDiskPlayer->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && multifileDiskPlayer->helpAnchor
                == "#bounded-multifile-disk-player"
            && talas::app::soundLibraryHasAction(
                multifileDiskPlayer->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                multifileDiskPlayer->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                multifileDiskPlayer->actions,
                talas::app::SoundLibraryAction::Audition),
        "C37 example should expose exact open, help, audition, and render authority");
    const auto memoryWriter = std::ranges::find(
        result.manifest->entries,
        "bounded-memory-writer-monitor",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        memoryWriter != result.manifest->entries.end()
            && memoryWriter->projectPath
                == "examples/c38_bounded_memory_writer.talas.json"
            && memoryWriter->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && memoryWriter->helpAnchor
                == "#bounded-memory-writer-monitor"
            && talas::app::soundLibraryHasAction(
                memoryWriter->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                memoryWriter->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                memoryWriter->actions,
                talas::app::SoundLibraryAction::Audition),
        "C38 example should expose exact open, help, audition, and render authority");
    const auto rawMidi = std::ranges::find(
        result.manifest->entries,
        "bounded-raw-midi-output",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        rawMidi != result.manifest->entries.end()
            && rawMidi->projectPath
                == "examples/c39_bounded_raw_midi_output.talas.json"
            && rawMidi->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && rawMidi->helpAnchor == "#bounded-raw-midi-output"
            && talas::app::soundLibraryHasAction(
                rawMidi->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                rawMidi->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                rawMidi->actions,
                talas::app::SoundLibraryAction::Audition),
        "C39 example should expose exact open, help, terminal audition, and render authority");
    const auto midiController = std::ranges::find(
        result.manifest->entries,
        "bounded-midi-controller-output",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        midiController != result.manifest->entries.end()
            && midiController->projectPath
                == "examples/c40_bounded_midi_controller_output.talas.json"
            && midiController->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && midiController->helpAnchor
                == "#bounded-midi-controller-output"
            && talas::app::soundLibraryHasAction(
                midiController->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                midiController->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                midiController->actions,
                talas::app::SoundLibraryAction::Audition),
        "C40 example should expose exact open, help, terminal audition, and render authority");
    const auto midiNote = std::ranges::find(
        result.manifest->entries,
        "bounded-midi-note-output",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        midiNote != result.manifest->entries.end()
            && midiNote->projectPath
                == "examples/c41_bounded_midi_note_output.talas.json"
            && midiNote->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && midiNote->helpAnchor == "#bounded-midi-note-output"
            && talas::app::soundLibraryHasAction(
                midiNote->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                midiNote->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                midiNote->actions,
                talas::app::SoundLibraryAction::Audition),
        "C41 example should expose exact open, help, terminal audition, and render authority");
    const auto midiFile = std::ranges::find(
        result.manifest->entries,
        "bounded-midi-file-echo",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        midiFile != result.manifest->entries.end()
            && midiFile->projectPath
                == "examples/c42_bounded_midi_file_echo.talas.json"
            && midiFile->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && midiFile->helpAnchor == "#bounded-midi-file-echo"
            && talas::app::soundLibraryHasAction(
                midiFile->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                midiFile->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                midiFile->actions,
                talas::app::SoundLibraryAction::Audition),
        "C42 example should expose exact open, help, terminal audition, and render authority");
    const auto globalController = std::ranges::find(
        result.manifest->entries,
        "bounded-global-controller-output",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        globalController != result.manifest->entries.end()
            && globalController->projectPath
                == "examples/c43_bounded_global_controller_output.talas.json"
            && globalController->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && globalController->helpAnchor
                == "#bounded-global-controller-output"
            && talas::app::soundLibraryHasAction(
                globalController->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                globalController->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                globalController->actions,
                talas::app::SoundLibraryAction::Audition),
        "C43 example should expose exact open, help, terminal audition, and render authority");
    const auto dynamicControls = std::ranges::find(
        result.manifest->entries,
        "dynamic-control-utilities",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        dynamicControls != result.manifest->entries.end()
            && dynamicControls->projectPath
                == "examples/c44_dynamic_control_utilities.talas.json"
            && dynamicControls->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && dynamicControls->helpAnchor
                == "#dynamic-control-utilities"
            && talas::app::soundLibraryHasAction(
                dynamicControls->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                dynamicControls->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                dynamicControls->actions,
                talas::app::SoundLibraryAction::Audition),
        "C44 example should expose exact open, help, audition, and render authority");
    const auto dynamicSlew = std::ranges::find(
        result.manifest->entries,
        "dynamic-slew-times",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        dynamicSlew != result.manifest->entries.end()
            && dynamicSlew->projectPath
                == "examples/c45_dynamic_slew_times.talas.json"
            && dynamicSlew->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && dynamicSlew->helpAnchor == "#dynamic-slew-times"
            && talas::app::soundLibraryHasAction(
                dynamicSlew->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                dynamicSlew->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                dynamicSlew->actions,
                talas::app::SoundLibraryAction::Audition),
        "C45 example should expose exact open, help, audition, and render authority");
    const auto dynamicClock = std::ranges::find(
        result.manifest->entries,
        "dynamic-clock-parameters",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        dynamicClock != result.manifest->entries.end()
            && dynamicClock->projectPath
                == "examples/c46_dynamic_clock_parameters.talas.json"
            && dynamicClock->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && dynamicClock->helpAnchor
                == "#dynamic-clock-parameters"
            && talas::app::soundLibraryHasAction(
                dynamicClock->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                dynamicClock->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                dynamicClock->actions,
                talas::app::SoundLibraryAction::Audition),
        "C46 example should expose exact open, help, audition, and render authority");
    const auto dynamicClockDivider = std::ranges::find(
        result.manifest->entries,
        "dynamic-clock-divider-parameters",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        dynamicClockDivider != result.manifest->entries.end()
            && dynamicClockDivider->projectPath
                == "examples/c47_dynamic_clock_divider_parameters.talas.json"
            && dynamicClockDivider->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && dynamicClockDivider->helpAnchor
                == "#dynamic-clock-divider-parameters"
            && talas::app::soundLibraryHasAction(
                dynamicClockDivider->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                dynamicClockDivider->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                dynamicClockDivider->actions,
                talas::app::SoundLibraryAction::Audition),
        "C47 example should expose exact open, help, audition, and render authority");
    const auto dynamicGlobalController = std::ranges::find(
        result.manifest->entries,
        "dynamic-global-controller-fallback",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        dynamicGlobalController != result.manifest->entries.end()
            && dynamicGlobalController->projectPath
                == "examples/c48_dynamic_global_controller_fallback.talas.json"
            && dynamicGlobalController->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && dynamicGlobalController->helpAnchor
                == "#dynamic-global-controller-fallback"
            && talas::app::soundLibraryHasAction(
                dynamicGlobalController->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                dynamicGlobalController->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                dynamicGlobalController->actions,
                talas::app::SoundLibraryAction::Audition),
        "C48 example should expose exact open, help, terminal audition, and render authority");
    const auto liveChanneller = std::ranges::find(
        result.manifest->entries,
        "live-channeller-selectors",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        liveChanneller != result.manifest->entries.end()
            && liveChanneller->projectPath
                == "examples/c49_live_channeller_selectors.talas.json"
            && liveChanneller->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && liveChanneller->helpAnchor
                == "#live-channeller-selectors"
            && talas::app::soundLibraryHasAction(
                liveChanneller->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                liveChanneller->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                liveChanneller->actions,
                talas::app::SoundLibraryAction::Audition),
        "C49 example should expose exact open, help, audition, and render authority");
    const auto liveSpectrumModifier = std::ranges::find(
        result.manifest->entries,
        "live-spectrum-modifier-selectors",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        liveSpectrumModifier != result.manifest->entries.end()
            && liveSpectrumModifier->projectPath
                == "examples/c50_live_spectrum_modifier_selectors.talas.json"
            && liveSpectrumModifier->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && liveSpectrumModifier->helpAnchor
                == "#live-spectrum-modifier-selectors"
            && talas::app::soundLibraryHasAction(
                liveSpectrumModifier->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                liveSpectrumModifier->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                liveSpectrumModifier->actions,
                talas::app::SoundLibraryAction::Audition),
        "C50 example should expose exact open, help, audition, and render authority");
    const auto liveSpectrumSeed = std::ranges::find(
        result.manifest->entries,
        "live-spectrum-modifier-seed",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        liveSpectrumSeed != result.manifest->entries.end()
            && liveSpectrumSeed->projectPath
                == "examples/c51_live_spectrum_modifier_seed.talas.json"
            && liveSpectrumSeed->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && liveSpectrumSeed->helpAnchor
                == "#live-spectrum-modifier-seed"
            && talas::app::soundLibraryHasAction(
                liveSpectrumSeed->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                liveSpectrumSeed->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                liveSpectrumSeed->actions,
                talas::app::SoundLibraryAction::Audition),
        "C51 example should expose exact open, help, audition, and render authority");
    const auto liveSpectrumHysteresis = std::ranges::find(
        result.manifest->entries,
        "live-spectrum-modifier-hysteresis",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        liveSpectrumHysteresis != result.manifest->entries.end()
            && liveSpectrumHysteresis->projectPath
                == "examples/c52_live_spectrum_modifier_hysteresis.talas.json"
            && liveSpectrumHysteresis->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && liveSpectrumHysteresis->helpAnchor
                == "#live-spectrum-modifier-hysteresis"
            && talas::app::soundLibraryHasAction(
                liveSpectrumHysteresis->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                liveSpectrumHysteresis->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                liveSpectrumHysteresis->actions,
                talas::app::SoundLibraryAction::Audition),
        "C52 example should expose exact open, help, audition, and render authority");
    const auto probabilityStream = std::ranges::find(
        result.manifest->entries,
        "spectrum-modifier-probability-stream",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        probabilityStream != result.manifest->entries.end()
            && probabilityStream->projectPath
                == "examples/c53_spectrum_modifier_probability_stream.talas.json"
            && probabilityStream->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && probabilityStream->helpAnchor
                == "#spectrum-modifier-probability-stream"
            && talas::app::soundLibraryHasAction(
                probabilityStream->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                probabilityStream->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                probabilityStream->actions,
                talas::app::SoundLibraryAction::Audition),
        "C53 example should expose exact open, help, audition, and render authority");
    const auto trackProbabilityProfile = std::ranges::find(
        result.manifest->entries,
        "spectrum-modifier-track-probability-profile",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        trackProbabilityProfile != result.manifest->entries.end()
            && trackProbabilityProfile->projectPath
                == "examples/c54_spectrum_modifier_track_probability_profile.talas.json"
            && trackProbabilityProfile->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && trackProbabilityProfile->helpAnchor
                == "#spectrum-modifier-track-probability-profile"
            && talas::app::soundLibraryHasAction(
                trackProbabilityProfile->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                trackProbabilityProfile->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                trackProbabilityProfile->actions,
                talas::app::SoundLibraryAction::Audition),
        "C54 example should expose exact open, help, audition, and render authority");
    const auto remainderProbabilityProfile = std::ranges::find(
        result.manifest->entries,
        "spectrum-modifier-remainder-probability-profile",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        remainderProbabilityProfile != result.manifest->entries.end()
            && remainderProbabilityProfile->projectPath
                == "examples/c55_spectrum_modifier_remainder_probability_profile.talas.json"
            && remainderProbabilityProfile->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && remainderProbabilityProfile->helpAnchor
                == "#spectrum-modifier-remainder-probability-profile"
            && talas::app::soundLibraryHasAction(
                remainderProbabilityProfile->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                remainderProbabilityProfile->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                remainderProbabilityProfile->actions,
                talas::app::SoundLibraryAction::Audition),
        "C55 example should expose exact open, help, audition, and render authority");
    const auto spectrumFrameScan = std::ranges::find(
        result.manifest->entries,
        "spectrum-on-disk-frame-scan",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        spectrumFrameScan != result.manifest->entries.end()
            && spectrumFrameScan->projectPath
                == "examples/c56_spectrum_on_disk_frame_scan.talas.json"
            && spectrumFrameScan->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && spectrumFrameScan->helpAnchor
                == "#spectrum-on-disk-frame-scan"
            && talas::app::soundLibraryHasAction(
                spectrumFrameScan->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                spectrumFrameScan->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                spectrumFrameScan->actions,
                talas::app::SoundLibraryAction::Audition),
        "C56 example should expose exact open, help, audition, and render authority");
    const auto voicedUnvoicedTransition = std::ranges::find(
        result.manifest->entries,
        "spectrum-voiced-unvoiced-transition",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        voicedUnvoicedTransition != result.manifest->entries.end()
            && voicedUnvoicedTransition->projectPath
                == "examples/c57_spectrum_voiced_unvoiced_transition.talas.json"
            && voicedUnvoicedTransition->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && voicedUnvoicedTransition->helpAnchor
                == "#spectrum-voiced-unvoiced-transition"
            && talas::app::soundLibraryHasAction(
                voicedUnvoicedTransition->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                voicedUnvoicedTransition->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                voicedUnvoicedTransition->actions,
                talas::app::SoundLibraryAction::Audition),
        "C57 example should expose exact open, help, audition, and render authority");
    const auto normalizedTimeIndex = std::ranges::find(
        result.manifest->entries,
        "spectrum-in-ram-normalized-time-index",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        normalizedTimeIndex != result.manifest->entries.end()
            && normalizedTimeIndex->projectPath
                == "examples/c58_spectrum_in_ram_normalized_time_index.talas.json"
            && normalizedTimeIndex->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && normalizedTimeIndex->helpAnchor
                == "#spectrum-in-ram-normalized-timeindex"
            && talas::app::soundLibraryHasAction(
                normalizedTimeIndex->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                normalizedTimeIndex->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                normalizedTimeIndex->actions,
                talas::app::SoundLibraryAction::Audition),
        "C58 example should expose exact open, help, audition, and render authority");
    const auto defaultAnalysisMetadata = std::ranges::find(
        result.manifest->entries,
        "spectrum-in-ram-default-analysis-metadata",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        defaultAnalysisMetadata != result.manifest->entries.end()
            && defaultAnalysisMetadata->projectPath
                == "examples/c59_spectrum_in_ram_default_analysis_metadata.talas.json"
            && defaultAnalysisMetadata->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && defaultAnalysisMetadata->helpAnchor
                == "#spectrum-in-ram-default-analysis-metadata"
            && talas::app::soundLibraryHasAction(
                defaultAnalysisMetadata->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                defaultAnalysisMetadata->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                defaultAnalysisMetadata->actions,
                talas::app::SoundLibraryAction::Audition),
        "C59 example should expose exact open, help, audition, and render authority");
    const auto diskDefaultAnalysisMetadata = std::ranges::find(
        result.manifest->entries,
        "spectrum-on-disk-default-analysis-metadata",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        diskDefaultAnalysisMetadata != result.manifest->entries.end()
            && diskDefaultAnalysisMetadata->projectPath
                == "examples/c60_spectrum_on_disk_default_analysis_metadata.talas.json"
            && diskDefaultAnalysisMetadata->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && diskDefaultAnalysisMetadata->helpAnchor
                == "#spectrum-on-disk-default-analysis-metadata"
            && talas::app::soundLibraryHasAction(
                diskDefaultAnalysisMetadata->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                diskDefaultAnalysisMetadata->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                diskDefaultAnalysisMetadata->actions,
                talas::app::SoundLibraryAction::Audition),
        "C60 example should expose exact open, help, audition, and render authority");
    const auto sumDefaultAnalysisFrequencies = std::ranges::find(
        result.manifest->entries,
        "sum-of-sines-default-analysis-frequencies",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        sumDefaultAnalysisFrequencies != result.manifest->entries.end()
            && sumDefaultAnalysisFrequencies->projectPath
                == "examples/c61_sum_of_sines_default_analysis_frequencies.talas.json"
            && sumDefaultAnalysisFrequencies->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && sumDefaultAnalysisFrequencies->helpAnchor
                == "#sum-of-sines-default-analysis-frequencies"
            && talas::app::soundLibraryHasAction(
                sumDefaultAnalysisFrequencies->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                sumDefaultAnalysisFrequencies->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                sumDefaultAnalysisFrequencies->actions,
                talas::app::SoundLibraryAction::Audition),
        "C61 example should expose exact open, help, audition, and render authority");
    const auto sumControlledTimeFrames = std::ranges::find(
        result.manifest->entries,
        "sum-of-sines-controlled-time-frames",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        sumControlledTimeFrames != result.manifest->entries.end()
            && sumControlledTimeFrames->projectPath
                == "examples/c62_sum_of_sines_controlled_time_frames.talas.json"
            && sumControlledTimeFrames->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && sumControlledTimeFrames->helpAnchor
                == "#sum-of-sines-controlled-time-frames"
            && talas::app::soundLibraryHasAction(
                sumControlledTimeFrames->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                sumControlledTimeFrames->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                sumControlledTimeFrames->actions,
                talas::app::SoundLibraryAction::Audition),
        "C62 example should expose exact open, help, audition, and render authority");
    const auto sumAutomaticGateLoop = std::ranges::find(
        result.manifest->entries,
        "sum-of-sines-automatic-gate-loop",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        sumAutomaticGateLoop != result.manifest->entries.end()
            && sumAutomaticGateLoop->projectPath
                == "examples/c63_sum_of_sines_automatic_gate_loop.talas.json"
            && sumAutomaticGateLoop->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && sumAutomaticGateLoop->helpAnchor
                == "#sum-of-sines-automatic-gate-loop"
            && talas::app::soundLibraryHasAction(
                sumAutomaticGateLoop->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                sumAutomaticGateLoop->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                sumAutomaticGateLoop->actions,
                talas::app::SoundLibraryAction::Audition),
        "C63 example should expose exact open, help, audition, and render authority");
    const auto syntheticSpectrumArrayPrefix = std::ranges::find(
        result.manifest->entries,
        "synthetic-spectrum-array-prefix",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        syntheticSpectrumArrayPrefix != result.manifest->entries.end()
            && syntheticSpectrumArrayPrefix->projectPath
                == "examples/c64_synthetic_spectrum_array_prefix.talas.json"
            && syntheticSpectrumArrayPrefix->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && syntheticSpectrumArrayPrefix->helpAnchor
                == "#synthetic-spectrum-array-prefix"
            && talas::app::soundLibraryHasAction(
                syntheticSpectrumArrayPrefix->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                syntheticSpectrumArrayPrefix->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                syntheticSpectrumArrayPrefix->actions,
                talas::app::SoundLibraryAction::Audition),
        "C64 example should expose exact open, help, audition, and render authority");
    const auto syntheticSpectrumStaticExpressions = std::ranges::find(
        result.manifest->entries,
        "synthetic-spectrum-static-expressions",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        syntheticSpectrumStaticExpressions != result.manifest->entries.end()
            && syntheticSpectrumStaticExpressions->projectPath
                == "examples/c65_synthetic_spectrum_static_expressions.talas.json"
            && syntheticSpectrumStaticExpressions->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && syntheticSpectrumStaticExpressions->helpAnchor
                == "#synthetic-spectrum-static-expressions"
            && talas::app::soundLibraryHasAction(
                syntheticSpectrumStaticExpressions->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                syntheticSpectrumStaticExpressions->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                syntheticSpectrumStaticExpressions->actions,
                talas::app::SoundLibraryAction::Audition),
        "C65 example should expose exact open, help, audition, and render authority");
    const auto thresholdHysteresisGate = std::ranges::find(
        result.manifest->entries,
        "threshold-hysteresis-gate",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        thresholdHysteresisGate != result.manifest->entries.end()
            && thresholdHysteresisGate->projectPath
                == "examples/c66_threshold_hysteresis_gate.talas.json"
            && thresholdHysteresisGate->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && thresholdHysteresisGate->helpAnchor
                == "#threshold-hysteresis-gate"
            && talas::app::soundLibraryHasAction(
                thresholdHysteresisGate->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                thresholdHysteresisGate->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                thresholdHysteresisGate->actions,
                talas::app::SoundLibraryAction::Audition),
        "C66 example should expose exact open, help, audition, and render authority");
    const auto timeControlHalfSpeed = std::ranges::find(
        result.manifest->entries,
        "time-control-half-speed",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        timeControlHalfSpeed != result.manifest->entries.end()
            && timeControlHalfSpeed->projectPath
                == "examples/c67_time_control_half_speed.talas.json"
            && timeControlHalfSpeed->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && timeControlHalfSpeed->helpAnchor
                == "#time-control-half-speed"
            && talas::app::soundLibraryHasAction(
                timeControlHalfSpeed->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                timeControlHalfSpeed->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                timeControlHalfSpeed->actions,
                talas::app::SoundLibraryAction::Audition),
        "C67 example should expose exact open, help, audition, and render authority");
    const auto timeStopperSampleHold = std::ranges::find(
        result.manifest->entries,
        "time-stopper-sample-hold",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        timeStopperSampleHold != result.manifest->entries.end()
            && timeStopperSampleHold->projectPath
                == "examples/c68_time_stopper_sample_hold.talas.json"
            && timeStopperSampleHold->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && timeStopperSampleHold->helpAnchor
                == "#time-stopper-sample-hold"
            && talas::app::soundLibraryHasAction(
                timeStopperSampleHold->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                timeStopperSampleHold->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                timeStopperSampleHold->actions,
                talas::app::SoundLibraryAction::Audition),
        "C68 example should expose exact open, help, audition, and render authority");
    const auto timeOffsetDelayLine = std::ranges::find(
        result.manifest->entries,
        "time-offset-delay-line",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        timeOffsetDelayLine != result.manifest->entries.end()
            && timeOffsetDelayLine->projectPath
                == "examples/c69_time_offset_delay_line.talas.json"
            && timeOffsetDelayLine->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && timeOffsetDelayLine->helpAnchor
                == "#time-offset-delay-line"
            && talas::app::soundLibraryHasAction(
                timeOffsetDelayLine->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                timeOffsetDelayLine->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                timeOffsetDelayLine->actions,
                talas::app::SoundLibraryAction::Audition),
        "C69 example should expose exact open, help, audition, and render authority");
    const auto triggeredSampleAndHoldEdges = std::ranges::find(
        result.manifest->entries,
        "triggered-sample-and-hold-edges",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        triggeredSampleAndHoldEdges != result.manifest->entries.end()
            && triggeredSampleAndHoldEdges->projectPath
                == "examples/c70_triggered_sample_and_hold_edges.talas.json"
            && triggeredSampleAndHoldEdges->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && triggeredSampleAndHoldEdges->helpAnchor
                == "#triggered-sample-and-hold-edges"
            && talas::app::soundLibraryHasAction(
                triggeredSampleAndHoldEdges->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                triggeredSampleAndHoldEdges->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                triggeredSampleAndHoldEdges->actions,
                talas::app::SoundLibraryAction::Audition),
        "C70 example should expose exact open, help, audition, and render authority");
    const auto triggeredTableReadGateReset = std::ranges::find(
        result.manifest->entries,
        "triggered-table-read-gate-reset",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        triggeredTableReadGateReset != result.manifest->entries.end()
            && triggeredTableReadGateReset->projectPath
                == "examples/c71_triggered_table_read_gate_reset.talas.json"
            && triggeredTableReadGateReset->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && triggeredTableReadGateReset->helpAnchor
                == "#triggered-table-read-gate-reset"
            && talas::app::soundLibraryHasAction(
                triggeredTableReadGateReset->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                triggeredTableReadGateReset->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                triggeredTableReadGateReset->actions,
                talas::app::SoundLibraryAction::Audition),
        "C71 example should expose exact open, help, audition, and render authority");
    const auto tunableVocoderBankReset = std::ranges::find(
        result.manifest->entries,
        "tunable-vocoder-bank-reset",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        tunableVocoderBankReset != result.manifest->entries.end()
            && tunableVocoderBankReset->projectPath
                == "examples/c72_tunable_vocoder_bank_reset.talas.json"
            && tunableVocoderBankReset->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && tunableVocoderBankReset->helpAnchor
                == "#tunable-vocoder-bank-reset"
            && talas::app::soundLibraryHasAction(
                tunableVocoderBankReset->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                tunableVocoderBankReset->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                tunableVocoderBankReset->actions,
                talas::app::SoundLibraryAction::Audition),
        "C72 example should expose exact open, help, audition, and render authority");
    const auto twoFormantPairReset = std::ranges::find(
        result.manifest->entries,
        "two-formant-pair-reset",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        twoFormantPairReset != result.manifest->entries.end()
            && twoFormantPairReset->projectPath
                == "examples/c73_two_formant_pair_reset.talas.json"
            && twoFormantPairReset->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && twoFormantPairReset->helpAnchor
                == "#two-formant-pair-reset"
            && talas::app::soundLibraryHasAction(
                twoFormantPairReset->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                twoFormantPairReset->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                twoFormantPairReset->actions,
                talas::app::SoundLibraryAction::Audition),
        "C73 example should expose exact open, help, audition, and render authority");
    const auto soundVariableSelectorReset = std::ranges::find(
        result.manifest->entries,
        "sound-variable-selector-reset",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        soundVariableSelectorReset != result.manifest->entries.end()
            && soundVariableSelectorReset->projectPath
                == "examples/c74_sound_variable_selector_reset.talas.json"
            && soundVariableSelectorReset->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && soundVariableSelectorReset->helpAnchor
                == "#sound-variable-selector-reset"
            && talas::app::soundLibraryHasAction(
                soundVariableSelectorReset->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                soundVariableSelectorReset->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                soundVariableSelectorReset->actions,
                talas::app::SoundLibraryAction::Audition),
        "C74 example should expose exact open, help, audition, and render authority");
    const auto fixedVocoderBankReset = std::ranges::find(
        result.manifest->entries,
        "fixed-vocoder-bank-reset",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        fixedVocoderBankReset != result.manifest->entries.end()
            && fixedVocoderBankReset->projectPath
                == "examples/c75_fixed_vocoder_bank_reset.talas.json"
            && fixedVocoderBankReset->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && fixedVocoderBankReset->helpAnchor
                == "#fixed-vocoder-bank-reset"
            && talas::app::soundLibraryHasAction(
                fixedVocoderBankReset->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                fixedVocoderBankReset->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                fixedVocoderBankReset->actions,
                talas::app::SoundLibraryAction::Audition),
        "C75 example should expose exact open, help, audition, and render authority");
    const auto vocoderChannelBankReset = std::ranges::find(
        result.manifest->entries,
        "vocoder-channel-bank-reset",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        vocoderChannelBankReset != result.manifest->entries.end()
            && vocoderChannelBankReset->projectPath
                == "examples/c76_vocoder_channel_bank_reset.talas.json"
            && vocoderChannelBankReset->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && vocoderChannelBankReset->helpAnchor
                == "#vocoder-channel-bank-reset"
            && talas::app::soundLibraryHasAction(
                vocoderChannelBankReset->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                vocoderChannelBankReset->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                vocoderChannelBankReset->actions,
                talas::app::SoundLibraryAction::Audition),
        "C76 example should expose exact open, help, audition, and render authority");
    const auto waitUntilBufferedResume = std::ranges::find(
        result.manifest->entries,
        "wait-until-buffered-resume",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        waitUntilBufferedResume != result.manifest->entries.end()
            && waitUntilBufferedResume->projectPath
                == "examples/c77_wait_until_buffered_resume.talas.json"
            && waitUntilBufferedResume->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && waitUntilBufferedResume->helpAnchor
                == "#wait-until-buffered-resume"
            && talas::app::soundLibraryHasAction(
                waitUntilBufferedResume->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                waitUntilBufferedResume->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                waitUntilBufferedResume->actions,
                talas::app::SoundLibraryAction::Audition),
        "C77 example should expose exact open, help, audition, and render authority");
    const auto warpedTimeIndexRestart = std::ranges::find(
        result.manifest->entries,
        "warped-time-index-restart",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        warpedTimeIndexRestart != result.manifest->entries.end()
            && warpedTimeIndexRestart->projectPath
                == "examples/c78_warped_time_index_restart.talas.json"
            && warpedTimeIndexRestart->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && warpedTimeIndexRestart->helpAnchor
                == "#warped-time-index-restart"
            && talas::app::soundLibraryHasAction(
                warpedTimeIndexRestart->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                warpedTimeIndexRestart->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                warpedTimeIndexRestart->actions,
                talas::app::SoundLibraryAction::Audition),
        "C78 example should expose exact open, help, audition, and render authority");
    const auto waveshaperPolynomialCurve = std::ranges::find(
        result.manifest->entries,
        "waveshaper-polynomial-curve",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        waveshaperPolynomialCurve != result.manifest->entries.end()
            && waveshaperPolynomialCurve->projectPath
                == "examples/c79_waveshaper_polynomial_curve.talas.json"
            && waveshaperPolynomialCurve->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && waveshaperPolynomialCurve->helpAnchor
                == "#waveshaper-polynomial-curve"
            && talas::app::soundLibraryHasAction(
                waveshaperPolynomialCurve->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                waveshaperPolynomialCurve->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                waveshaperPolynomialCurve->actions,
                talas::app::SoundLibraryAction::Audition),
        "C79 example should expose exact open, help, audition, and render authority");
    const auto iteratedWaveshaperFeedback = std::ranges::find(
        result.manifest->entries,
        "iterated-waveshaper-feedback",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        iteratedWaveshaperFeedback != result.manifest->entries.end()
            && iteratedWaveshaperFeedback->projectPath
                == "examples/c80_iterated_waveshaper_feedback.talas.json"
            && iteratedWaveshaperFeedback->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && iteratedWaveshaperFeedback->helpAnchor
                == "#iterated-waveshaper-feedback"
            && talas::app::soundLibraryHasAction(
                iteratedWaveshaperFeedback->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                iteratedWaveshaperFeedback->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                iteratedWaveshaperFeedback->actions,
                talas::app::SoundLibraryAction::Audition),
        "C80 example should expose exact open, help, audition, and render authority");
    const auto multiplyingWaveshaperProduct = std::ranges::find(
        result.manifest->entries,
        "multiplying-waveshaper-product",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        multiplyingWaveshaperProduct != result.manifest->entries.end()
            && multiplyingWaveshaperProduct->projectPath
                == "examples/c81_multiplying_waveshaper_product.talas.json"
            && multiplyingWaveshaperProduct->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && multiplyingWaveshaperProduct->helpAnchor
                == "#multiplying-waveshaper-product"
            && talas::app::soundLibraryHasAction(
                multiplyingWaveshaperProduct->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                multiplyingWaveshaperProduct->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                multiplyingWaveshaperProduct->actions,
                talas::app::SoundLibraryAction::Audition),
        "C81 example should expose exact open, help, audition, and render authority");
    const auto inputOutputCharacteristicCurve = std::ranges::find(
        result.manifest->entries,
        "input-output-characteristic-curve",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        inputOutputCharacteristicCurve != result.manifest->entries.end()
            && inputOutputCharacteristicCurve->projectPath
                == "examples/c82_input_output_characteristic_curve.talas.json"
            && inputOutputCharacteristicCurve->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && inputOutputCharacteristicCurve->helpAnchor
                == "#input-output-characteristic-curve"
            && talas::app::soundLibraryHasAction(
                inputOutputCharacteristicCurve->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                inputOutputCharacteristicCurve->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                inputOutputCharacteristicCurve->actions,
                talas::app::SoundLibraryAction::Audition),
        "C82 example should expose exact open, help, audition, and render authority");
    const auto scaleAndOffsetAffineControl = std::ranges::find(
        result.manifest->entries,
        "scale-and-offset-affine-control",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        scaleAndOffsetAffineControl != result.manifest->entries.end()
            && scaleAndOffsetAffineControl->projectPath
                == "examples/c83_scale_and_offset_affine_control.talas.json"
            && scaleAndOffsetAffineControl->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && scaleAndOffsetAffineControl->helpAnchor
                == "#scale-and-offset-affine-control"
            && talas::app::soundLibraryHasAction(
                scaleAndOffsetAffineControl->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                scaleAndOffsetAffineControl->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                scaleAndOffsetAffineControl->actions,
                talas::app::SoundLibraryAction::Audition),
        "C83 example should expose exact open, help, audition, and render authority");
    const auto interpolateDualLaneControl = std::ranges::find(
        result.manifest->entries,
        "interpolate-dual-lane-control",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        interpolateDualLaneControl != result.manifest->entries.end()
            && interpolateDualLaneControl->projectPath
                == "examples/c84_interpolate_dual_lane_control.talas.json"
            && interpolateDualLaneControl->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && interpolateDualLaneControl->helpAnchor
                == "#interpolate-dual-lane-control"
            && talas::app::soundLibraryHasAction(
                interpolateDualLaneControl->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                interpolateDualLaneControl->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                interpolateDualLaneControl->actions,
                talas::app::SoundLibraryAction::Audition),
        "C84 example should expose exact open, help, audition, and render authority");
    const auto runningMaxResetControl = std::ranges::find(
        result.manifest->entries,
        "running-max-reset-control",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        runningMaxResetControl != result.manifest->entries.end()
            && runningMaxResetControl->projectPath
                == "examples/c85_running_max_reset_control.talas.json"
            && runningMaxResetControl->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && runningMaxResetControl->helpAnchor
                == "#running-max-reset-control"
            && talas::app::soundLibraryHasAction(
                runningMaxResetControl->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                runningMaxResetControl->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                runningMaxResetControl->actions,
                talas::app::SoundLibraryAction::Audition),
        "C85 example should expose exact open, help, audition, and render authority");
    const auto runningMinResetBaseline = std::ranges::find(
        result.manifest->entries,
        "running-min-reset-baseline",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        runningMinResetBaseline != result.manifest->entries.end()
            && runningMinResetBaseline->projectPath
                == "examples/c86_running_min_reset_baseline.talas.json"
            && runningMinResetBaseline->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && runningMinResetBaseline->helpAnchor
                == "#running-min-reset-baseline"
            && talas::app::soundLibraryHasAction(
                runningMinResetBaseline->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                runningMinResetBaseline->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                runningMinResetBaseline->actions,
                talas::app::SoundLibraryAction::Audition),
        "C86 example should expose exact open, help, audition, and render authority");
    const auto rmsSquaredEnvelope = std::ranges::find(
        result.manifest->entries,
        "rms-squared-envelope",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        rmsSquaredEnvelope != result.manifest->entries.end()
            && rmsSquaredEnvelope->projectPath
                == "examples/c87_rms_squared_envelope.talas.json"
            && rmsSquaredEnvelope->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && rmsSquaredEnvelope->helpAnchor
                == "#rms-squared-envelope"
            && talas::app::soundLibraryHasAction(
                rmsSquaredEnvelope->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                rmsSquaredEnvelope->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                rmsSquaredEnvelope->actions,
                talas::app::SoundLibraryAction::Audition),
        "C87 example should expose exact open, help, audition, and render authority");
    const auto amplitudeFollowerEnvelope = std::ranges::find(
        result.manifest->entries,
        "amplitude-follower-envelope",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        amplitudeFollowerEnvelope != result.manifest->entries.end()
            && amplitudeFollowerEnvelope->projectPath
                == "examples/c88_amplitude_follower_envelope.talas.json"
            && amplitudeFollowerEnvelope->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && amplitudeFollowerEnvelope->helpAnchor
                == "#amplitude-follower-envelope"
            && talas::app::soundLibraryHasAction(
                amplitudeFollowerEnvelope->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                amplitudeFollowerEnvelope->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                amplitudeFollowerEnvelope->actions,
                talas::app::SoundLibraryAction::Audition),
        "C88 example should expose exact open, help, audition, and render authority");
    const auto peakDetectorEnvelope = std::ranges::find(
        result.manifest->entries,
        "peak-detector-envelope",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        peakDetectorEnvelope != result.manifest->entries.end()
            && peakDetectorEnvelope->projectPath
                == "examples/c89_peak_detector_envelope.talas.json"
            && peakDetectorEnvelope->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && peakDetectorEnvelope->helpAnchor
                == "#peak-detector-envelope"
            && talas::app::soundLibraryHasAction(
                peakDetectorEnvelope->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                peakDetectorEnvelope->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                peakDetectorEnvelope->actions,
                talas::app::SoundLibraryAction::Audition),
        "C89 example should expose exact open, help, audition, and render authority");
    const auto frequencyTrackerHistory = std::ranges::find(
        result.manifest->entries,
        "frequency-tracker-history",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        frequencyTrackerHistory != result.manifest->entries.end()
            && frequencyTrackerHistory->projectPath
                == "examples/c90_frequency_tracker_history.talas.json"
            && frequencyTrackerHistory->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && frequencyTrackerHistory->helpAnchor
                == "#frequency-tracker-history"
            && talas::app::soundLibraryHasAction(
                frequencyTrackerHistory->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                frequencyTrackerHistory->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                frequencyTrackerHistory->actions,
                talas::app::SoundLibraryAction::Audition),
        "C90 example should expose exact open, help, audition, and render authority");
    const auto arEnvelopeState = std::ranges::find(
        result.manifest->entries,
        "ar-envelope-state",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        arEnvelopeState != result.manifest->entries.end()
            && arEnvelopeState->projectPath
                == "examples/c91_ar_envelope_state.talas.json"
            && arEnvelopeState->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && arEnvelopeState->helpAnchor == "#ar-envelope-state"
            && talas::app::soundLibraryHasAction(
                arEnvelopeState->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                arEnvelopeState->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                arEnvelopeState->actions,
                talas::app::SoundLibraryAction::Audition),
        "C91 example should expose exact open, help, audition, and render authority");
    const auto adsrGateState = std::ranges::find(
        result.manifest->entries,
        "adsr-gate-state",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        adsrGateState != result.manifest->entries.end()
            && adsrGateState->projectPath
                == "examples/c92_adsr_typed_gate_state.talas.json"
            && adsrGateState->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && adsrGateState->helpAnchor == "#adsr-gate-state"
            && talas::app::soundLibraryHasAction(
                adsrGateState->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                adsrGateState->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                adsrGateState->actions,
                talas::app::SoundLibraryAction::Audition),
        "C92 example should expose exact open, help, audition, and render authority");
    const auto pulseTrainPhaseState = std::ranges::find(
        result.manifest->entries,
        "pulse-train-phase-state",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        pulseTrainPhaseState != result.manifest->entries.end()
            && pulseTrainPhaseState->projectPath
                == "examples/c93_pulse_train_phase_state.talas.json"
            && pulseTrainPhaseState->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && pulseTrainPhaseState->helpAnchor
                == "#pulse-train-phase-state"
            && talas::app::soundLibraryHasAction(
                pulseTrainPhaseState->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                pulseTrainPhaseState->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                pulseTrainPhaseState->actions,
                talas::app::SoundLibraryAction::Audition),
        "C93 example should expose exact open, help, audition, and render authority");
    const auto pulseGeneratorPhaseState = std::ranges::find(
        result.manifest->entries,
        "pulse-generator-phase-state",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        pulseGeneratorPhaseState != result.manifest->entries.end()
            && pulseGeneratorPhaseState->projectPath
                == "examples/c94_pulse_generator_phase_state.talas.json"
            && pulseGeneratorPhaseState->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && pulseGeneratorPhaseState->helpAnchor
                == "#pulse-generator-phase-state"
            && talas::app::soundLibraryHasAction(
                pulseGeneratorPhaseState->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                pulseGeneratorPhaseState->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                pulseGeneratorPhaseState->actions,
                talas::app::SoundLibraryAction::Audition),
        "C94 example should expose exact open, help, audition, and render authority");
    const auto oscillatorPhaseState = std::ranges::find(
        result.manifest->entries,
        "oscillator-phase-state",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        oscillatorPhaseState != result.manifest->entries.end()
            && oscillatorPhaseState->projectPath
                == "examples/c95_oscillator_phase_state.talas.json"
            && oscillatorPhaseState->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && oscillatorPhaseState->helpAnchor
                == "#oscillator-phase-state"
            && talas::app::soundLibraryHasAction(
                oscillatorPhaseState->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                oscillatorPhaseState->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                oscillatorPhaseState->actions,
                talas::app::SoundLibraryAction::Audition),
        "C95 example should expose exact open, help, audition, and render authority");
    const auto lfoPhaseTransport = std::ranges::find(
        result.manifest->entries,
        "lfo-phase-transport",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        lfoPhaseTransport != result.manifest->entries.end()
            && lfoPhaseTransport->projectPath
                == "examples/c96_lfo_phase_transport.talas.json"
            && lfoPhaseTransport->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && lfoPhaseTransport->helpAnchor
                == "#lfo-phase-and-transport"
            && talas::app::soundLibraryHasAction(
                lfoPhaseTransport->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                lfoPhaseTransport->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                lfoPhaseTransport->actions,
                talas::app::SoundLibraryAction::Audition),
        "C96 example should expose exact open, help, audition, and render authority");
    const auto rampTriggerTransport = std::ranges::find(
        result.manifest->entries,
        "ramp-trigger-transport",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        rampTriggerTransport != result.manifest->entries.end()
            && rampTriggerTransport->projectPath
                == "examples/c97_ramp_trigger_transport.talas.json"
            && rampTriggerTransport->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && rampTriggerTransport->helpAnchor
                == "#ramp-trigger-and-transport"
            && talas::app::soundLibraryHasAction(
                rampTriggerTransport->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                rampTriggerTransport->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                rampTriggerTransport->actions,
                talas::app::SoundLibraryAction::Audition),
        "C97 example should expose exact open, help, audition, and render authority");
    const auto sampleAndHoldClockTransport = std::ranges::find(
        result.manifest->entries,
        "sample-and-hold-clock-transport",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        sampleAndHoldClockTransport != result.manifest->entries.end()
            && sampleAndHoldClockTransport->projectPath
                == "examples/c98_sample_and_hold_clock_transport.talas.json"
            && sampleAndHoldClockTransport->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && sampleAndHoldClockTransport->helpAnchor
                == "#sample-and-hold-clock-and-transport"
            && talas::app::soundLibraryHasAction(
                sampleAndHoldClockTransport->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                sampleAndHoldClockTransport->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                sampleAndHoldClockTransport->actions,
                talas::app::SoundLibraryAction::Audition),
        "C98 example should expose exact open, help, audition, and render authority");
    const auto samplesFromDiskSingleStepTransport = std::ranges::find(
        result.manifest->entries,
        "samples-from-disk-single-step-transport",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        samplesFromDiskSingleStepTransport
                != result.manifest->entries.end()
            && samplesFromDiskSingleStepTransport->projectPath
                == "examples/c99_samples_from_disk_single_step_transport.talas.json"
            && samplesFromDiskSingleStepTransport->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && samplesFromDiskSingleStepTransport->helpAnchor
                == "#samples-from-disk-single-step-transport"
            && talas::app::soundLibraryHasAction(
                samplesFromDiskSingleStepTransport->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                samplesFromDiskSingleStepTransport->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                samplesFromDiskSingleStepTransport->actions,
                talas::app::SoundLibraryAction::Audition),
        "C99 example should expose exact open, help, audition, and render authority");
    const auto phaseShiftBy90AllpassTransport = std::ranges::find(
        result.manifest->entries,
        "phase-shift-by-90-allpass-transport",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        phaseShiftBy90AllpassTransport
                != result.manifest->entries.end()
            && phaseShiftBy90AllpassTransport->projectPath
                == "examples/c100_phase_shift_by_90_allpass_transport.talas.json"
            && phaseShiftBy90AllpassTransport->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && phaseShiftBy90AllpassTransport->helpAnchor
                == "#phase-shift-by-90-allpass-transport"
            && talas::app::soundLibraryHasAction(
                phaseShiftBy90AllpassTransport->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                phaseShiftBy90AllpassTransport->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                phaseShiftBy90AllpassTransport->actions,
                talas::app::SoundLibraryAction::Audition),
        "C100 example should expose exact open, help, audition, and render authority");
    const auto singleSideBandRmQuadratureTransport = std::ranges::find(
        result.manifest->entries,
        "single-side-band-rm-quadrature-transport",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        singleSideBandRmQuadratureTransport
                != result.manifest->entries.end()
            && singleSideBandRmQuadratureTransport->projectPath
                == "examples/c101_single_side_band_rm_quadrature_transport.talas.json"
            && singleSideBandRmQuadratureTransport->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && singleSideBandRmQuadratureTransport->helpAnchor
                == "#single-side-band-rm-quadrature-transport"
            && talas::app::soundLibraryHasAction(
                singleSideBandRmQuadratureTransport->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                singleSideBandRmQuadratureTransport->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                singleSideBandRmQuadratureTransport->actions,
                talas::app::SoundLibraryAction::Audition),
        "C101 example should expose exact open, help, audition, and render authority");
    const auto sosOscillatorsSelectedWavetableTransport =
        std::ranges::find(
            result.manifest->entries,
            "sos-oscillators-selected-wavetable-transport",
            &talas::app::SoundLibraryManifestEntry::id);
    require(
        sosOscillatorsSelectedWavetableTransport
                != result.manifest->entries.end()
            && sosOscillatorsSelectedWavetableTransport->projectPath
                == "examples/c102_sos_oscillators_selected_wavetable_transport.talas.json"
            && sosOscillatorsSelectedWavetableTransport->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && sosOscillatorsSelectedWavetableTransport->helpAnchor
                == "#sos-oscillators-selected-wavetable-transport"
            && talas::app::soundLibraryHasAction(
                sosOscillatorsSelectedWavetableTransport->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                sosOscillatorsSelectedWavetableTransport->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                sosOscillatorsSelectedWavetableTransport->actions,
                talas::app::SoundLibraryAction::Audition),
        "C102 example should expose exact open, help, audition, and render authority");
    const auto lowPassGateControlStateTransport = std::ranges::find(
        result.manifest->entries,
        "low-pass-gate-control-state-transport",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        lowPassGateControlStateTransport != result.manifest->entries.end()
            && lowPassGateControlStateTransport->projectPath
                == "examples/c103_low_pass_gate_control_state_transport.talas.json"
            && lowPassGateControlStateTransport->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && lowPassGateControlStateTransport->helpAnchor
                == "#low-pass-gate-control-state-transport"
            && talas::app::soundLibraryHasAction(
                lowPassGateControlStateTransport->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                lowPassGateControlStateTransport->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                lowPassGateControlStateTransport->actions,
                talas::app::SoundLibraryAction::Audition),
        "C103 example should expose exact open, help, audition, and render authority");
    const auto reverbSectionFractionalDelayTransport = std::ranges::find(
        result.manifest->entries,
        "reverb-section-fractional-delay-transport",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        reverbSectionFractionalDelayTransport
                != result.manifest->entries.end()
            && reverbSectionFractionalDelayTransport->projectPath
                == "examples/c104_reverb_section_fractional_delay_transport.talas.json"
            && reverbSectionFractionalDelayTransport->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && reverbSectionFractionalDelayTransport->helpAnchor
                == "#reverb-section-fractional-delay-transport"
            && talas::app::soundLibraryHasAction(
                reverbSectionFractionalDelayTransport->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                reverbSectionFractionalDelayTransport->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                reverbSectionFractionalDelayTransport->actions,
                talas::app::SoundLibraryAction::Audition),
        "C104 example should expose exact open, help, audition, and render authority");
    const auto monotonizerKnownPitchTransport = std::ranges::find(
        result.manifest->entries,
        "monotonizer-known-pitch-transport",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        monotonizerKnownPitchTransport != result.manifest->entries.end()
            && monotonizerKnownPitchTransport->projectPath
                == "examples/c105_monotonizer_known_pitch_transport.talas.json"
            && monotonizerKnownPitchTransport->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && monotonizerKnownPitchTransport->helpAnchor
                == "#monotonizer-known-pitch-transport"
            && talas::app::soundLibraryHasAction(
                monotonizerKnownPitchTransport->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                monotonizerKnownPitchTransport->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                monotonizerKnownPitchTransport->actions,
                talas::app::SoundLibraryAction::Audition),
        "C105 example should expose exact open, help, audition, and render authority");
    const auto multifileDiskPlayerSelectionTransport = std::ranges::find(
        result.manifest->entries,
        "multifile-disk-player-selection-transport",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        multifileDiskPlayerSelectionTransport
                != result.manifest->entries.end()
            && multifileDiskPlayerSelectionTransport->projectPath
                == "examples/c106_multifile_disk_player_selection_transport.talas.json"
            && multifileDiskPlayerSelectionTransport->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && multifileDiskPlayerSelectionTransport->helpAnchor
                == "#multifile-disk-player-selection-transport"
            && talas::app::soundLibraryHasAction(
                multifileDiskPlayerSelectionTransport->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                multifileDiskPlayerSelectionTransport->actions,
                talas::app::SoundLibraryAction::OpenHelp)
            && talas::app::soundLibraryHasAction(
                multifileDiskPlayerSelectionTransport->actions,
                talas::app::SoundLibraryAction::Audition),
        "C106 example should expose exact open, help, audition, and render authority");
    for (const auto [id, helpAnchor] : {
             std::pair{
                 std::string_view{"paired-stereo-output-4"},
                 std::string_view{"#paired-stereo-output-4"}},
             std::pair{
                 std::string_view{"paired-stereo-output-8"},
                 std::string_view{"#paired-stereo-output-8"}},
         })
    {
        const auto pairedTerminal = std::ranges::find(
            result.manifest->entries,
            id,
            &talas::app::SoundLibraryManifestEntry::id);
        require(
            pairedTerminal != result.manifest->entries.end()
                && pairedTerminal->expectedOutcome
                    == talas::app::SoundLibraryExpectedOutcome::Renderable
                && pairedTerminal->helpAnchor == helpAnchor
                && talas::app::soundLibraryHasAction(
                    pairedTerminal->actions,
                    talas::app::SoundLibraryAction::OpenExample)
                && talas::app::soundLibraryHasAction(
                    pairedTerminal->actions,
                    talas::app::SoundLibraryAction::Audition),
            std::string{id}
                + " should expose exact C28 open, audition, render, and local-help authority");
    }
    const auto formantBankOscillator = std::ranges::find(
        result.manifest->entries,
        "bounded-formant-bank-oscillator",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        formantBankOscillator != result.manifest->entries.end()
            && formantBankOscillator->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && formantBankOscillator->helpAnchor
                == "#bounded-formant-bank-oscillator"
            && talas::app::soundLibraryHasAction(
                formantBankOscillator->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                formantBankOscillator->actions,
                talas::app::SoundLibraryAction::Audition),
        "bounded Formant Bank Oscillator example should expose exact C26 open, audition, render, and local-help authority");
    const auto formantBank = std::ranges::find(
        result.manifest->entries,
        "bounded-formant-bank",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        formantBank != result.manifest->entries.end()
            && formantBank->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && formantBank->helpAnchor
                == "#bounded-formant-bank"
            && talas::app::soundLibraryHasAction(
                formantBank->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                formantBank->actions,
                talas::app::SoundLibraryAction::Audition),
        "bounded Formant Bank example should expose exact C25 open, audition, render, and local-help authority");
    const auto oscillatorBank = std::ranges::find(
        result.manifest->entries,
        "bounded-oscillator-bank",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        oscillatorBank != result.manifest->entries.end()
            && oscillatorBank->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && oscillatorBank->helpAnchor
                == "#bounded-oscillator-bank"
            && talas::app::soundLibraryHasAction(
                oscillatorBank->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                oscillatorBank->actions,
                talas::app::SoundLibraryAction::Audition),
        "bounded Oscillator Bank example should expose exact C24 open, audition, render, and local-help authority");
    const auto overlappingMixer = std::ranges::find(
        result.manifest->entries,
        "bounded-overlapping-mixer",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        overlappingMixer != result.manifest->entries.end()
            && overlappingMixer->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && overlappingMixer->helpAnchor
                == "#bounded-overlapping-mixer"
            && talas::app::soundLibraryHasAction(
                overlappingMixer->actions,
                talas::app::SoundLibraryAction::OpenExample)
            && talas::app::soundLibraryHasAction(
                overlappingMixer->actions,
                talas::app::SoundLibraryAction::Audition),
        "bounded Overlapping Mixer example should expose exact C23 open, audition, render, and local-help authority");
    const auto dynamicRangeController = std::ranges::find(
        result.manifest->entries,
        "bounded-dynamic-range-controller",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        dynamicRangeController != result.manifest->entries.end()
            && dynamicRangeController->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && dynamicRangeController->helpAnchor
                == "#bounded-dynamic-range-controller"
            && talas::app::soundLibraryHasAction(
                dynamicRangeController->actions,
                talas::app::SoundLibraryAction::Audition),
        "bounded dynamic range controller example should expose its exact prepared renderable audition and local help");
    const auto harmonicResonator = std::ranges::find(
        result.manifest->entries,
        "bounded-harmonic-resonator",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        harmonicResonator != result.manifest->entries.end()
            && harmonicResonator->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && harmonicResonator->helpAnchor
                == "#bounded-harmonic-resonator"
            && talas::app::soundLibraryHasAction(
                harmonicResonator->actions,
                talas::app::SoundLibraryAction::Audition),
        "bounded harmonic resonator example should expose its exact prepared renderable audition and local help");
    const auto vcf = std::ranges::find(
        result.manifest->entries,
        "checked-four-pole-tpt-vcf",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        vcf != result.manifest->entries.end()
            && vcf->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && talas::app::soundLibraryHasAction(
                vcf->actions,
                talas::app::SoundLibraryAction::Audition),
        "checked four-pole TPT VCF example should expose its prepared renderable audition");
    const auto staticFir = std::ranges::find(
        result.manifest->entries,
        "bounded-static-fir-family",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        staticFir != result.manifest->entries.end()
            && staticFir->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && talas::app::soundLibraryHasAction(
                staticFir->actions,
                talas::app::SoundLibraryAction::Audition),
        "bounded static FIR example should expose its exact asset-backed prepared renderable audition");
    const auto checkedBiquad = std::ranges::find(
        result.manifest->entries,
        "checked-biquad-family",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        checkedBiquad != result.manifest->entries.end()
            && checkedBiquad->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && talas::app::soundLibraryHasAction(
                checkedBiquad->actions,
                talas::app::SoundLibraryAction::Audition),
        "checked Biquad family example should expose its exact prepared renderable audition");
    const auto shelvingPresence = std::ranges::find(
        result.manifest->entries,
        "shelving-presence-filters",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        shelvingPresence != result.manifest->entries.end()
            && shelvingPresence->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && talas::app::soundLibraryHasAction(
                shelvingPresence->actions,
                talas::app::SoundLibraryAction::Audition),
        "shelving/presence example should expose its exact prepared renderable audition");
    const auto stereoRouting = std::ranges::find(
        result.manifest->entries,
        "stereo-routing-utilities",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        stereoRouting != result.manifest->entries.end()
            && stereoRouting->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && talas::app::soundLibraryHasAction(
                stereoRouting->actions,
                talas::app::SoundLibraryAction::Audition),
        "stereo-routing example should expose its exact prepared renderable audition");
    const auto analogSequencer = std::ranges::find(
        result.manifest->entries,
        "bounded-analog-sequencer",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        analogSequencer != result.manifest->entries.end()
            && analogSequencer->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && talas::app::soundLibraryHasAction(
                analogSequencer->actions,
                talas::app::SoundLibraryAction::Audition),
        "bounded Analog Sequencer example should expose its prepared renderable audition");
    const auto diskCache = std::ranges::find(
        result.manifest->entries,
        "bounded-disk-cache-capture",
        &talas::app::SoundLibraryManifestEntry::id);
    require(
        diskCache != result.manifest->entries.end()
            && diskCache->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && talas::app::soundLibraryHasAction(
                diskCache->actions,
                talas::app::SoundLibraryAction::Audition),
        "bounded Disk Cache example should expose its verified renderable capture");
    const auto feedback = std::ranges::find(
        result.manifest->entries,
        "reusable-feedback-help",
        &talas::app::SoundLibraryManifestEntry::id);
    require(feedback != result.manifest->entries.end()
            && feedback->definitionId
                == std::optional<std::string>{"feedbackVoice"}
            && talas::app::soundLibraryHasAction(
                feedback->actions,
                talas::app::SoundLibraryAction::UseInProject)
            && !talas::app::soundLibraryHasAction(
                feedback->actions,
                talas::app::SoundLibraryAction::Audition),
        "validation-only reusable example should expose exact import but not audition");
    require(talas::project::isCanonicalProjectAssetContentHash(
                feedback->verifiedProjectContentHash),
        "checked manifest load should bind every example to exact bounded bytes");
    const auto snapshot = talas::app::loadSoundLibraryExampleSnapshot(
        std::filesystem::path{TALAS_SOURCE_DIR}, *feedback, registry);
    require(snapshot.ok()
            && snapshot.snapshot->contentHash
                == feedback->verifiedProjectContentHash,
        "example snapshot should re-read and match the verified project digest");
    auto staleFeedback = *feedback;
    staleFeedback.verifiedProjectContentHash.assign(64, '0');
    require(!talas::app::loadSoundLibraryExampleSnapshot(
                 std::filesystem::path{TALAS_SOURCE_DIR},
                 staleFeedback,
                 registry)
                 .ok(),
        "example snapshot should reject bytes that do not match retained authority");
    const auto streaming = std::ranges::find(
        result.manifest->entries,
        "immutable-streaming-health",
        &talas::app::SoundLibraryManifestEntry::id);
    require(streaming != result.manifest->entries.end(),
        "checked manifest should retain the streaming asset example");
    const auto streamingSnapshot = talas::app::loadSoundLibraryExampleSnapshot(
        std::filesystem::path{TALAS_SOURCE_DIR}, *streaming, registry);
    require(streamingSnapshot.ok()
            && streamingSnapshot.snapshot->document.assets.size() == 1
            && talas::project::isCanonicalProjectAssetContentHash(
                streamingSnapshot.snapshot->document.assets.front().contentHash),
        "example snapshots should retain inspected asset-byte authority");
    require(streaming->auditionAudioAssetIds
            == std::vector<std::string>{
                streamingSnapshot.snapshot->document.assets.front().id},
        "checked manifest should derive exact frozen-audio choices from the verified snapshot");
    for (const auto& entry : result.manifest->entries)
    {
        if (!entry.definitionId.has_value())
            continue;
        const auto exact = talas::app::loadSoundLibraryExampleSnapshot(
            std::filesystem::path{TALAS_SOURCE_DIR}, entry, registry);
        require(exact.ok(), "definition-bearing example should reload exactly");
        const auto definition = std::ranges::find(
            exact.snapshot->document.soundDefinitions,
            *entry.definitionId,
            &talas::project::ProjectSoundDefinition::definitionId);
        require(definition != exact.snapshot->document.soundDefinitions.end()
                && entry.auditionInputs == definition->inputs,
            "manifest authority should derive audition inputs only from the exact reusable definition");
    }
    const auto preserved = std::ranges::find(
        result.manifest->entries,
        "missing-component-preservation",
        &talas::app::SoundLibraryManifestEntry::id);
    require(preserved != result.manifest->entries.end()
            && preserved->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::PreservedUnavailable
            && preserved->actions
                == (talas::app::soundLibraryActionMask(
                        talas::app::SoundLibraryAction::OpenExample)
                    | talas::app::soundLibraryActionMask(
                        talas::app::SoundLibraryAction::OpenHelp)),
        "preserved example should remain inspectable but non-executable");
}

void testExampleSnapshotRequiresRetainedAssetByteAuthority()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto sourceRoot = std::filesystem::path{TALAS_SOURCE_DIR};
    const auto sourceProject =
        fileText(sourceRoot / "examples/m3_streaming_health.talas.json");
    auto parsed = talas::project::parseProjectDocument(sourceProject);
    require(parsed.ok() && parsed.document.assets.size() == 1,
        "streaming fixture should provide one referenced asset");
    parsed.document.assets.front().contentHash.clear();
    const auto projectText =
        talas::project::serializeProjectDocument(parsed.document);

    TemporaryDirectory directory;
    const auto projectPath =
        directory.path() / "examples/asset-authority.talas.json";
    writeText(projectPath, projectText);
    std::filesystem::copy_file(
        sourceRoot / "examples/resources/m3-stream-source.wav",
        directory.path() / "examples/resources/m3-stream-source.wav");

    talas::app::SoundLibraryManifestEntry entry;
    entry.id = "asset-authority";
    entry.projectPath = "examples/asset-authority.talas.json";
    entry.expectedOutcome = talas::app::SoundLibraryExpectedOutcome::Renderable;
    entry.verifiedProjectContentHash =
        talas::project::calculateProjectAssetSha256(
            std::span<const std::uint8_t>{
                reinterpret_cast<const std::uint8_t*>(projectText.data()),
                projectText.size()});
    require(!talas::app::loadSoundLibraryExampleSnapshot(
                 directory.path(), entry, registry)
                 .ok(),
        "repository examples should reject referenced assets without retained byte authority");
}

void testExampleSnapshotRejectsWindowsRootAndDriveRelativeAssets()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto sourceRoot = std::filesystem::path{TALAS_SOURCE_DIR};
    const auto sourceProject =
        fileText(sourceRoot / "examples/m3_streaming_health.talas.json");
    const auto sourceAsset =
        sourceRoot / "examples/resources/m3-stream-source.wav";

    for (const auto* rejectedPath : {
             "\\outside.wav",
             "C:outside.wav",
         })
    {
        auto parsed = talas::project::parseProjectDocument(sourceProject);
        require(parsed.ok() && parsed.document.assets.size() == 1,
            "streaming fixture should provide one referenced asset");
        parsed.document.assets.front().path = rejectedPath;
        const auto projectText =
            talas::project::serializeProjectDocument(parsed.document);

        TemporaryDirectory directory;
        writeText(
            directory.path() / "examples/platform-relative.talas.json",
            projectText);
#if !defined(_WIN32)
        std::filesystem::copy_file(
            sourceAsset,
            directory.path() / "examples" / rejectedPath);
#endif

        talas::app::SoundLibraryManifestEntry entry;
        entry.id = "platform-relative";
        entry.projectPath = "examples/platform-relative.talas.json";
        entry.expectedOutcome =
            talas::app::SoundLibraryExpectedOutcome::Renderable;
        entry.verifiedProjectContentHash =
            talas::project::calculateProjectAssetSha256(
                std::span<const std::uint8_t>{
                    reinterpret_cast<const std::uint8_t*>(projectText.data()),
                    projectText.size()});
        require(!talas::app::loadSoundLibraryExampleSnapshot(
                     directory.path(), entry, registry)
                     .ok(),
            "repository examples should reject Windows root- and drive-relative asset paths on every platform");
    }
}

void testManifestParserIsClosedAndFailClosed()
{
    const auto canonical = manifestText();
    require(talas::app::parseSoundLibraryManifest(canonical).ok(),
        "canonical manifest text should parse");
    for (const auto& invalid : {
             replacedOnce(canonical, "\"entries\"", "\"unknown\""),
             replacedOnce(canonical,
                 "examples/m3_live_spectral_analysis.talas.json",
                 "examples/../m3_live_spectral_analysis.talas.json"),
             replacedOnce(canonical,
                 "[\"openExample\", \"openHelp\", \"audition\"]",
                 "[\"openExample\", \"openHelp\", \"audition\", \"audition\"]"),
             replacedOnce(canonical,
                 "\"expectedOutcome\": \"renderable\",\n"
                 "      \"definitionId\": null,\n"
                 "      \"actions\": [\"openExample\", \"openHelp\", \"audition\"]",
                 "\"expectedOutcome\": \"validation-only\",\n"
                 "      \"definitionId\": null,\n"
                 "      \"actions\": [\"openExample\", \"openHelp\", \"audition\"]"),
             replacedOnce(canonical,
                 "\"schemaVersion\": 1",
                 "\"schemaVersion\": 1.0"),
             replacedOnce(canonical,
                 "\"title\": \"Live Spectral Analysis\"",
                 "\"title\": \"" + std::string(161, 'A') + "\""),
             replacedOnce(canonical,
                 "Audible oscillator analysis",
                 "Audible\\noscillator analysis"),
         })
    {
        require(!talas::app::parseSoundLibraryManifest(invalid).ok(),
            "unknown, traversal, duplicate, contradictory, and non-exact schema evidence should reject");
    }
}

void testCheckedManifestRejectsLocalHelpAndIndexDrift()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto source = std::filesystem::path{TALAS_SOURCE_DIR};
    TemporaryDirectory directory;
    std::filesystem::create_directories(directory.path() / "docs/help");
    std::filesystem::copy(
        source / "examples",
        directory.path() / "examples",
        std::filesystem::copy_options::recursive
            | std::filesystem::copy_options::overwrite_existing);
    for (const auto& helpFile : {
             "node-example-index.json",
             "builtin-node-reference.md",
         })
    {
        std::filesystem::copy_file(
            source / "docs/help" / helpFile,
            directory.path() / "docs/help" / helpFile,
            std::filesystem::copy_options::overwrite_existing);
    }
    require(talas::app::loadSoundLibraryManifest(
                directory.path(), registry)
                .ok(),
        "copied checked corpus should retain exact local authority");

    const auto readme = directory.path() / "examples/README.md";
    const auto canonicalReadme = fileText(readme);
    writeText(readme, replacedOnce(
        canonicalReadme,
        "## Other focused examples",
        "## Renamed focused examples"));
    require(!talas::app::loadSoundLibraryManifest(
                 directory.path(), registry)
                 .ok(),
        "manifest should reject a help anchor removed from local README authority");
    writeText(readme, canonicalReadme);

    std::string headingsOnly;
    std::size_t offset = 0;
    while (offset < canonicalReadme.size())
    {
        const auto end = canonicalReadme.find('\n', offset);
        const auto line = canonicalReadme.substr(
            offset,
            (end == std::string::npos ? canonicalReadme.size() : end)
                - offset);
        if (line.starts_with('#'))
            headingsOnly += line + "\n";
        if (end == std::string::npos)
            break;
        offset = end + 1;
    }
    writeText(readme, headingsOnly);
    require(!talas::app::loadSoundLibraryManifest(
                 directory.path(), registry)
                 .ok(),
        "manifest should reject help retaining anchors but losing exact local non-claim authority");
    writeText(readme, canonicalReadme);

    const auto reference =
        directory.path() / "docs/help/builtin-node-reference.md";
    const auto canonicalReference = fileText(reference);
    writeText(reference, "# Incomplete help\n");
    require(!talas::app::loadSoundLibraryManifest(
                 directory.path(), registry)
                 .ok(),
        "manifest should reject missing checked builtin help/non-claim evidence");
    writeText(reference, canonicalReference);
    writeText(reference, replacedOnce(
        canonicalReference,
        "<code>talas.annotation</code>",
        "<code>talas.annotation.missing</code>"));
    require(!talas::app::loadSoundLibraryManifest(
                 directory.path(), registry)
                 .ok(),
        "manifest should reject one missing built-in identity even when help markers and non-claims remain intact");
    writeText(reference, canonicalReference);

    const auto index =
        directory.path() / "docs/help/node-example-index.json";
    const auto canonicalIndex = fileText(index);
    writeText(index, replacedOnce(
        canonicalIndex,
        "examples/m3_live_spectral_analysis.talas.json",
        "examples/unindexed-project.talas.json"));
    require(!talas::app::loadSoundLibraryManifest(
                 directory.path(), registry)
                 .ok(),
        "manifest should reject generated index path drift");
    writeText(index, replacedOnce(
        canonicalIndex,
        "tools/report_node_help.py",
        "tools/untrusted_generator.py"));
    require(!talas::app::loadSoundLibraryManifest(
                 directory.path(), registry)
                 .ok(),
        "manifest should reject generated-index authority drift even when paths remain unchanged");
    writeText(index, replacedOnce(
        canonicalIndex,
        "\"expectedOutcome\": \"renderable\"",
        "\"expectedOutcome\": \"validation-only\""));
    require(!talas::app::loadSoundLibraryManifest(
                 directory.path(), registry)
                 .ok(),
        "manifest should reject an indexed outcome that disagrees with the checked manifest and project");
    writeText(index, replacedOnce(
        canonicalIndex,
        "        \"talas.liveSpectralAnalysis\",\n",
        ""));
    require(!talas::app::loadSoundLibraryManifest(
                 directory.path(), registry)
                 .ok(),
        "manifest should reject an incomplete registered-node evidence set");
    writeText(index, replacedOnce(
        canonicalIndex,
        "        \"org.talas.example.spectral-colour\"\n",
        ""));
    require(!talas::app::loadSoundLibraryManifest(
                 directory.path(), registry)
                 .ok(),
        "manifest should reject an incomplete preserved-node evidence set");
    writeText(index, canonicalIndex);

    TemporaryDirectory symlinkRoot;
    std::filesystem::create_directories(symlinkRoot.path() / "docs");
    std::error_code symlinkError;
    std::filesystem::create_directory_symlink(
        directory.path() / "examples",
        symlinkRoot.path() / "examples",
        symlinkError);
    if (!symlinkError)
    {
        std::filesystem::create_directory_symlink(
            directory.path() / "docs/help",
            symlinkRoot.path() / "docs/help",
            symlinkError);
    }
    if (!symlinkError)
    {
        require(!talas::app::loadSoundLibraryManifest(
                     symlinkRoot.path(), registry)
                     .ok(),
            "manifest should reject any parent-directory symlink that escapes the explicit root");
    }

    TemporaryDirectory helpSymlinkRoot;
    std::filesystem::copy(
        directory.path() / "examples",
        helpSymlinkRoot.path() / "examples",
        std::filesystem::copy_options::recursive
            | std::filesystem::copy_options::overwrite_existing);
    std::filesystem::create_directories(helpSymlinkRoot.path() / "docs");
    symlinkError.clear();
    std::filesystem::create_directory_symlink(
        directory.path() / "docs/help",
        helpSymlinkRoot.path() / "docs/help",
        symlinkError);
    if (!symlinkError)
    {
        require(!talas::app::loadSoundLibraryManifest(
                     helpSymlinkRoot.path(), registry)
                     .ok(),
            "manifest should reject a symlinked local-help parent outside the explicit root");
    }

    const auto externalResources = directory.path() / "external-resources";
    std::filesystem::rename(
        directory.path() / "examples/resources", externalResources);
    symlinkError.clear();
    std::filesystem::create_directory_symlink(
        externalResources,
        directory.path() / "examples/resources",
        symlinkError);
    if (!symlinkError)
    {
        require(!talas::app::loadSoundLibraryManifest(
                     directory.path(), registry)
                     .ok(),
            "manifest should reject a declared asset reached through a symlinked parent");
    }
}

constexpr std::array demoEffects{
    std::string_view{"auto_filter"},
    std::string_view{"bus_compressor"},
    std::string_view{"chorus"},
    std::string_view{"flanger"},
    std::string_view{"frequency_shifter"},
    std::string_view{"granular_freeze"},
    std::string_view{"hall_reverb"},
    std::string_view{"phaser"},
    std::string_view{"ring_modulator"},
    std::string_view{"spectral_blur"},
    std::string_view{"stereo_delay"},
    std::string_view{"tape_saturator"},
    std::string_view{"vocoder"},
};
constexpr std::array demoInstruments{
    std::string_view{"arpeggiator"},
    std::string_view{"drawbar_organ"},
    std::string_view{"drum_kit"},
    std::string_view{"fm_electric_piano"},
    std::string_view{"granular_synth"},
    std::string_view{"modal_mallets"},
    std::string_view{"morph_lead"},
    std::string_view{"plucked_string"},
    std::string_view{"poly_pad"},
    std::string_view{"sub_bass"},
    std::string_view{"terrain_drone"},
};

[[nodiscard]] std::string demoPath(std::string_view stem)
{
    return "examples/demos/" + std::string{stem} + ".talas.json";
}

// Every demo is listed once, as an effect or an instrument, and its exact
// project bytes load from the given root as an openable document.
void requireDemosListedAndOpenable(
    const std::filesystem::path& root,
    const talas::core::NodeRegistry& registry,
    const talas::app::SoundLibraryManifest& manifest)
{
    std::size_t demoCount = 0;
    for (const auto& entry : manifest.entries)
    {
        if (entry.projectPath.starts_with(talas::app::soundLibraryDemoFolder))
            ++demoCount;
    }
    require(demoCount == demoEffects.size() + demoInstruments.size(),
        "the manifest should list exactly the twenty-four demos");
    const auto check = [&](std::string_view stem,
                           talas::app::SoundLibraryDemoKind kind) {
        const auto path = demoPath(stem);
        const auto found = std::ranges::find(
            manifest.entries, path,
            &talas::app::SoundLibraryManifestEntry::projectPath);
        require(found != manifest.entries.end(),
            "demo " + path + " should be listed in the Sound Library");
        const auto effect = kind == talas::app::SoundLibraryDemoKind::Effect;
        auto expectedId = "demo-" + std::string{stem};
        std::ranges::replace(expectedId, '_', '-');
        require(found->demoKind == kind
                && found->category == talas::app::soundLibraryDemoCategory
                && found->id == expectedId
                && !found->title.empty() && !found->summary.empty()
                && found->summary.size() <= 160
                && found->helpAnchor == (effect ? "#effects" : "#instruments")
                && found->expectedOutcome
                    == talas::app::SoundLibraryExpectedOutcome::Renderable
                && !found->definitionId.has_value()
                && talas::app::soundLibraryHasAction(
                    found->actions, talas::app::SoundLibraryAction::OpenExample)
                && talas::app::soundLibraryHasAction(
                    found->actions, talas::app::SoundLibraryAction::OpenHelp)
                && talas::app::soundLibraryHasAction(
                       found->actions, talas::app::SoundLibraryAction::Audition)
                    == !effect,
            "demo " + path + " should be a Demos entry of its exact kind, "
            "with a one-line summary, open and help, and audition only for an instrument");
        const auto snapshot = talas::app::loadSoundLibraryExampleSnapshot(
            root, *found, registry);
        require(snapshot.ok()
                && snapshot.snapshot->document.metadata.title == found->title
                && snapshot.snapshot->contentHash
                    == found->verifiedProjectContentHash,
            "demo " + path + " should open as its exact checked project: "
                + snapshot.diagnostic);
    };
    for (const auto stem : demoEffects)
        check(stem, talas::app::SoundLibraryDemoKind::Effect);
    for (const auto stem : demoInstruments)
        check(stem, talas::app::SoundLibraryDemoKind::Instrument);
}

void testDemosFormOneGroupOfEffectsAndInstruments()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto root = std::filesystem::path{TALAS_SOURCE_DIR};
    const auto loaded = talas::app::loadSoundLibraryManifest(root, registry);
    require(loaded.ok(), "demo manifest should load: " + loaded.diagnostic);
    requireDemosListedAndOpenable(root, registry, *loaded.manifest);

    const auto entries = talas::app::buildSoundLibraryEntries(
        {}, registry, *loaded.manifest);
    const auto firstExample = std::ranges::find(
        entries,
        talas::app::SoundLibrarySourceKind::OriginalExample,
        &talas::app::SoundLibraryEntry::sourceKind);
    require(std::distance(firstExample, entries.end()) == 202,
        "every original example should follow the other sources");
    const auto demosEnd = firstExample
        + static_cast<std::ptrdiff_t>(
            demoEffects.size() + demoInstruments.size());
    const auto effectsEnd = firstExample
        + static_cast<std::ptrdiff_t>(demoEffects.size());
    require(std::all_of(firstExample, effectsEnd, [](const auto& entry) {
                return entry.demoKind == talas::app::SoundLibraryDemoKind::Effect;
            })
            && std::all_of(effectsEnd, demosEnd, [](const auto& entry) {
                return entry.demoKind
                    == talas::app::SoundLibraryDemoKind::Instrument;
            })
            && std::none_of(demosEnd, entries.end(), [](const auto& entry) {
                   return entry.demoKind
                       != talas::app::SoundLibraryDemoKind::None;
               })
            && std::is_sorted(firstExample, effectsEnd,
                [](const auto& left, const auto& right) {
                    return left.displayName < right.displayName;
                })
            && std::is_sorted(effectsEnd, demosEnd,
                [](const auto& left, const auto& right) {
                    return left.displayName < right.displayName;
                }),
        "the demos should lead the original examples, effects then instruments, each by name");
    const auto delay = std::ranges::find(
        entries, "example:demo-stereo-delay",
        &talas::app::SoundLibraryEntry::stableId);
    const auto pad = std::ranges::find(
        entries, "example:demo-poly-pad",
        &talas::app::SoundLibraryEntry::stableId);
    require(delay != entries.end() && pad != entries.end()
            && delay->category == "Demos" && pad->category == "Demos"
            && delay->actionDiagnostic.find("live audio input 1")
                != std::string::npos
            && !talas::app::soundLibraryHasAction(
                delay->actions, talas::app::SoundLibraryAction::Audition)
            && pad->actionDiagnostic.empty()
            && talas::app::soundLibraryHasAction(
                pad->actions, talas::app::SoundLibraryAction::Audition)
            && talas::app::soundLibraryEntryMatches(*delay, "demo effect delay")
            && talas::app::soundLibraryEntryMatches(*pad, "demos instrument pad")
            && !talas::app::soundLibraryEntryMatches(*pad, "demo effect")
            && talas::app::soundLibraryDemoKindLabel(delay->demoKind) == "Effect"
            && talas::app::soundLibraryDemoKindLabel(pad->demoKind) == "Instrument"
            && talas::app::soundLibraryDemoKindLabel(
                   talas::app::SoundLibraryDemoKind::None).empty(),
        "a demo effect should say it needs live input instead of auditioning, and an instrument should audition");
}

void testDemoManifestRulesFailClosed()
{
    const auto canonical = manifestText();
    const auto delayEntry = std::string{
        "\"projectPath\": \"examples/demos/stereo_delay.talas.json\""};
    const auto delayTags = std::string{
        "\"tags\": [\"demo\", \"effect\", \"live-input\", \"delay\"]"};
    const auto delayActions = std::string{
        "\"helpAnchor\": \"#effects\",\n"
        "      \"expectedOutcome\": \"renderable\",\n"
        "      \"definitionId\": null,\n"
        "      \"actions\": [\"openExample\", \"openHelp\"]"};
    require(canonical.find(delayEntry) != std::string::npos
            && canonical.find(delayTags) != std::string::npos
            && canonical.find(delayActions) != std::string::npos,
        "the Stereo Delay demo should be the mutation fixture");
    for (const auto& invalid : {
             replacedOnce(canonical, delayEntry,
                 "\"projectPath\": \"examples/demos/deeper/stereo_delay.talas.json\""),
             replacedOnce(canonical, delayEntry,
                 "\"projectPath\": \"examples/resources/stereo_delay.talas.json\""),
             replacedOnce(canonical,
                 "\"category\": \"Demos\"", "\"category\": \"Effects\""),
             replacedOnce(canonical, delayTags,
                 "\"tags\": [\"effect\", \"live-input\", \"delay\"]"),
             replacedOnce(canonical, delayTags,
                 "\"tags\": [\"demo\", \"live-input\", \"delay\"]"),
             replacedOnce(canonical, delayTags,
                 "\"tags\": [\"demo\", \"effect\", \"instrument\", \"delay\"]"),
             replacedOnce(canonical, delayActions,
                 "\"helpAnchor\": \"#effects\",\n"
                 "      \"expectedOutcome\": \"renderable\",\n"
                 "      \"definitionId\": null,\n"
                 "      \"actions\": [\"openExample\", \"openHelp\", \"audition\"]"),
             replacedOnce(canonical,
                 "\"category\": \"Filters\"", "\"category\": \"Demos\""),
             replacedOnce(canonical,
                 "\"tags\": [\"filters\", \"vcf\", \"tpt\", \"prepared-realtime\"]",
                 "\"tags\": [\"demo\", \"filters\", \"vcf\", \"tpt\"]"),
         })
    {
        require(!talas::app::parseSoundLibraryManifest(invalid).ok(),
            "a demo outside examples/demos/, outside the Demos category, without "
            "exactly one kind tag, or an effect offering a silent audition should reject");
    }

    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto source = std::filesystem::path{TALAS_SOURCE_DIR};
    TemporaryDirectory directory;
    std::filesystem::create_directories(directory.path() / "docs/help");
    std::filesystem::copy(
        source / "examples",
        directory.path() / "examples",
        std::filesystem::copy_options::recursive
            | std::filesystem::copy_options::overwrite_existing);
    for (const auto& helpFile : {
             "node-example-index.json",
             "builtin-node-reference.md",
         })
    {
        std::filesystem::copy_file(
            source / "docs/help" / helpFile,
            directory.path() / "docs/help" / helpFile,
            std::filesystem::copy_options::overwrite_existing);
    }
    const auto copied = talas::app::loadSoundLibraryManifest(
        directory.path(), registry);
    require(copied.ok(), "copied corpus should load its demos: "
            + copied.diagnostic);
    requireDemosListedAndOpenable(directory.path(), registry, *copied.manifest);

    const auto demoReadme = directory.path() / "examples/demos/README.md";
    const auto canonicalDemoReadme = fileText(demoReadme);
    writeText(demoReadme, replacedOnce(
        canonicalDemoReadme, "## Instruments", "## Synthesizers"));
    require(!talas::app::loadSoundLibraryManifest(directory.path(), registry).ok(),
        "a demo help anchor missing from the demos README should reject");
    std::filesystem::remove(demoReadme);
    require(!talas::app::loadSoundLibraryManifest(directory.path(), registry).ok(),
        "a missing demos README should reject");
    writeText(demoReadme, canonicalDemoReadme);

    const auto manifest =
        directory.path() / "examples/sound-library-manifest.json";
    writeText(manifest, replacedOnce(canonical, delayTags,
        "\"tags\": [\"demo\", \"instrument\", \"live-input\", \"delay\"]"));
    require(talas::app::parseSoundLibraryManifest(fileText(manifest)).ok()
            && !talas::app::loadSoundLibraryManifest(directory.path(), registry)
                    .ok(),
        "a demo reading live input should not pass as an instrument");
    writeText(manifest, canonical);

    const auto delayProject =
        directory.path() / "examples/demos/stereo_delay.talas.json";
    std::filesystem::remove(delayProject);
    require(!talas::app::loadSoundLibraryManifest(directory.path(), registry).ok(),
        "a listed demo missing from the library root should reject");
}

// The library the app bundle ships: every demo is there and opens.
void testBundledLibraryShipsTheDemos(const std::filesystem::path& bundleRoot)
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    require(std::filesystem::is_regular_file(
                bundleRoot / "examples/demos/README.md"),
        "the bundled Sound Library should carry the demos README");
    const auto loaded = talas::app::loadSoundLibraryManifest(bundleRoot, registry);
    require(loaded.ok(), "the bundled Sound Library should load: "
            + loaded.diagnostic);
    require(loaded.manifest->entries.size() == 202,
        "the bundled Sound Library should list every original example");
    requireDemosListedAndOpenable(bundleRoot, registry, *loaded.manifest);
}

talas::project::ProjectSoundDefinition projectVoice()
{
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "projectVoice";
    definition.displayName = "Warm Project Voice";
    definition.graph.nodes.push_back({"vca", "talas.vca", {}, {}});
    definition.inputs.push_back({
        "audioIn",
        talas::core::PortType::Audio,
        "vca",
        "audioIn",
    });
    definition.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "vca",
        "audioOut",
    });
    return definition;
}

void testProjectionMergesThreeAuthoritiesAndSearchesCapabilities()
{
    auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::core::NodeTypeDescriptor component;
    component.typeId = "org.example.component";
    component.displayName = "External Chorus";
    component.category = "Effects";
    component.outputs.push_back(
        {"audioOut", talas::core::PortType::Audio, false});
    component.provenance.vendor = "Example Audio";
    component.provenance.origin = talas::core::NodeOrigin::IsolatedComponent;
    component.capabilities.preparedRealtime =
        talas::core::ExecutionSupport::Supported;
    component.capabilities.preparedRealtimeBinding =
        talas::core::ExecutionBinding::IsolatedProcess;
    component.supportLevel = talas::core::NodeSupportLevel::FullRuntime;
    component.supportSummary = "Verified isolated realtime chorus.";
    registry.addNodeType(component);

    talas::project::ProjectDocument document;
    document.soundDefinitions.push_back(projectVoice());
    const auto loaded = talas::app::loadSoundLibraryManifest(
        std::filesystem::path{TALAS_SOURCE_DIR}, registry);
    require(loaded.ok(), "projection fixture manifest should load");
    const auto entries = talas::app::buildSoundLibraryEntries(
        document,
        registry,
        *loaded.manifest);
    require(entries.size() == 204,
        "projection should merge one definition, one eligible component, and "
        "two hundred two examples");

    const auto definition = std::ranges::find(
        entries,
        "project:projectVoice",
        &talas::app::SoundLibraryEntry::stableId);
    require(definition != entries.end()
            && definition->sourceKind
                == talas::app::SoundLibrarySourceKind::ProjectDefinition
            && definition->compatibility
                == talas::app::SoundLibraryCompatibility::Ready
            && talas::app::soundLibraryHasAction(
                definition->actions,
                talas::app::SoundLibraryAction::ShowDefinition)
            && talas::app::soundLibraryHasAction(
                definition->actions,
                talas::app::SoundLibraryAction::Audition)
            && definition->auditionInputs
                == document.soundDefinitions.front().inputs,
        "ready project definition should expose show/use/audition authority");
    const auto external = std::ranges::find(
        entries,
        "component:org.example.component",
        &talas::app::SoundLibraryEntry::stableId);
    require(external != entries.end()
            && external->sourceKind
                == talas::app::SoundLibrarySourceKind::Component
            && talas::app::soundLibraryEntryMatches(
                *external,
                "example isolatedprocess chorus"),
        "component projection should search vendor, binding capability, and name as one multi-token index");
    const auto example = std::ranges::find(
        entries,
        "example:immutable-streaming-health",
        &talas::app::SoundLibraryEntry::stableId);
    require(example != entries.end()
            && talas::app::soundLibraryEntryMatches(
                *example,
                "resources streaming asset-health")
            && !talas::app::soundLibraryEntryMatches(
                *example,
                "streaming oscillator"),
        "example search should require every normalized query token");
    require(example->verifiedProjectContentHash.has_value()
            && talas::project::isCanonicalProjectAssetContentHash(
                *example->verifiedProjectContentHash),
        "projected original examples should retain exact verified byte identity");

    auto unsupportedDocument = document;
    unsupportedDocument.soundDefinitions.front().inputs.front().type =
        talas::core::PortType::Event;
    const auto unsupportedEntries = talas::app::buildSoundLibraryEntries(
        unsupportedDocument,
        registry,
        *loaded.manifest);
    const auto unsupported = std::ranges::find(
        unsupportedEntries,
        "project:projectVoice",
        &talas::app::SoundLibraryEntry::stableId);
    require(unsupported != unsupportedEntries.end()
            && !talas::app::soundLibraryHasAction(
                unsupported->actions,
                talas::app::SoundLibraryAction::Audition)
            && unsupported->auditionInputs.size() == 1
            && unsupported->auditionInputs.front().type
                == talas::core::PortType::Event
            && unsupported->actionDiagnostic.find("Event")
                != std::string::npos,
        "unsupported typed inputs should remain visible before start while Audition stays disabled");
}
}

int main(int argc, char** argv)
{
    if (argc == 3 && std::string_view{argv[1]} == "--bundle-root")
    {
        testBundledLibraryShipsTheDemos(std::filesystem::path{argv[2]});
        return 0;
    }
    if (argc != 1)
        throw std::runtime_error("usage: [--bundle-root <path>]");
    testCheckedManifestLoadsExactOriginalCorpus();
    testExampleSnapshotRequiresRetainedAssetByteAuthority();
    testExampleSnapshotRejectsWindowsRootAndDriveRelativeAssets();
    testManifestParserIsClosedAndFailClosed();
    testCheckedManifestRejectsLocalHelpAndIndexDrift();
    testProjectionMergesThreeAuthoritiesAndSearchesCapabilities();
    testDemosFormOneGroupOfEffectsAndInstruments();
    testDemoManifestRulesFailClosed();
}
