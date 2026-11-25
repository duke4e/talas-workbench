#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/audio/WavReader.h"

namespace talas::app::workbench
{
// // the Tape Recorder in the Tools menu monitors the inputs, measures them,
// records them through its faders into the file chosen, and plays the
// recording back.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runTapeRecorderSmoke(const std::filesystem::path& smokeDirectory)
{
    const auto finish = [&](std::string error = {}) -> std::optional<std::string> {
        tapeRecorderWindow_.reset();
        tapeRecorderPanel_.reset();
        tapeRecorder_.resetMaxLevels();
        for (std::size_t channel = 0; channel < 2; ++channel)
        {
            tapeRecorder_.setInputLevel(channel, 1.0F);
            tapeRecorder_.setChannelEnabled(channel, true);
        }
        return error.empty() ? std::nullopt : std::optional<std::string>{std::move(error)};
    };

    const auto names = getMenuBarNames();
    const auto toolsMenu = getMenuForIndex(names.indexOf("Tools"), "Tools");
    juce::PopupMenu::MenuItemIterator first{toolsMenu};
    if (!first.next() || first.getItem().itemID != commandShowTapeRecorder)
        return finish("the Tools menu does not begin with the Tape Recorder");
    if (!commandManager_.invokeDirectly(commandShowTapeRecorder, false) || tapeRecorderPanel_ == nullptr
        || tapeRecorderWindow_ != nullptr)
        return finish("the Tape Recorder command did not make its panel (without a window off screen)");
    // A Workbench with a device runs the recorder in its callback; the
    // smoke drives it only without one.
    if (deviceManager_.getCurrentAudioDevice() != nullptr)
        return finish();
    auto& panel = *tapeRecorderPanel_;
    panel.setSampleRateForSmoke(48000.0);

    std::array<float, 256> left{};
    std::array<float, 256> right{};
    std::array<float, 256> outLeft{};
    std::array<float, 256> outRight{};
    const auto callback = [&](float leftLevel, float rightLevel) {
        left.fill(leftLevel);
        right.fill(rightLevel);
        outLeft.fill(0.0F);
        outRight.fill(0.0F);
        const std::array<const float*, 2> inputs{left.data(), right.data()};
        const std::array<float*, 2> outputs{outLeft.data(), outRight.data()};
        tapeRecorder_.process(inputs, outputs, left.size());
        panel.refreshForSmoke();
    };

    const auto destination = smokeDirectory / "tape recorder.aif";
    panel.setFileForSmoke(destination, talas::audio::TimelineRecordingFormat::Aiff, 24);
    panel.pressForSmoke(TapeRecorderButton::Record);
    if (panel.state() != talas::audio::TapeRecorderState::Monitoring || !tapeRecorder_.monitoring())
        return finish("record did not put the Tape Recorder into monitor mode");
    callback(0.5F, -0.25F);
    if (std::abs(outLeft[0] - 0.5F) > 1.0e-6F || panel.maxLevelText(0) != "Max(L) 0.500"
        || panel.maxLevelText(1) != "Max(R) 0.250")
        return finish("monitoring did not play and measure the inputs: " + panel.maxLevelText(0).toStdString());
    panel.pressForSmoke(TapeRecorderButton::Reset);
    if (panel.maxLevelText(0) != "Max(L) 0.000")
        return finish("Reset did not clear the Max levels");
    panel.setInputLevelForSmoke(1, 2.0);
    if (std::abs(tapeRecorder_.inputLevel(1) - 2.0F) > 1.0e-6F)
        return finish("the Input(R) fader did not boost the input");

    panel.pressForSmoke(TapeRecorderButton::Pause);
    if (panel.state() != talas::audio::TapeRecorderState::Recording)
        return finish("unclicking pause did not start the recording: " + panel.statusText().toStdString());
    callback(0.5F, 0.125F);
    callback(0.5F, 0.125F);
    panel.pressForSmoke(TapeRecorderButton::Pause);
    callback(0.9F, 0.9F);
    if (panel.state() != talas::audio::TapeRecorderState::RecordingPaused || tapeRecorder_.recordedFrames() != 512)
        return finish("pause did not pause the recording");
    panel.pressForSmoke(TapeRecorderButton::Play);
    callback(-0.25F, 0.25F);
    if (panel.counterText() != "0:00.0" || tapeRecorder_.recordedFrames() != 768)
        return finish("play did not resume the recording: " + panel.counterText().toStdString());
    panel.pressForSmoke(TapeRecorderButton::Stop);
    if (panel.state() != talas::audio::TapeRecorderState::Stopped || !panel.hasRecording())
        return finish("stop did not end the recording: " + panel.statusText().toStdString());

    const auto file = talas::audio::readSampleFile(destination);
    if (!file.ok() || file.channels.size() != 2 || file.channels[0].size() != 768
        || std::abs(file.channels[1][0] - 0.25) > 1.0e-5 || std::abs(file.channels[0][767] + 0.25) > 1.0e-5)
        return finish("the recording was not saved as a stereo AIFF file: " + file.error);

    panel.pressForSmoke(TapeRecorderButton::Play);
    callback(0.0F, 0.0F);
    if (panel.state() != talas::audio::TapeRecorderState::Playing || std::abs(outRight[0] - 0.25F) > 1.0e-5F)
        return finish("play did not play the recording back");
    for (int block = 0; block < 3; ++block)
        callback(0.0F, 0.0F);
    if (panel.state() != talas::audio::TapeRecorderState::Stopped)
        return finish("the playback did not stop at the recording's end");

    panel.setChannelForSmoke(0, false);
    panel.pressForSmoke(TapeRecorderButton::Record);
    panel.pressForSmoke(TapeRecorderButton::Play);
    callback(0.5F, -0.125F);
    panel.pressForSmoke(TapeRecorderButton::Stop);
    const auto mono = talas::audio::readSampleFile(destination);
    if (!mono.ok() || mono.channels.size() != 1 || mono.channels[0].size() != 256
        || std::abs(mono.channels[0][0] + 0.25) > 1.0e-5)
        return finish("recording again over the file did not record the Right input alone: " + mono.error);
    std::error_code ignored;
    std::filesystem::remove(destination, ignored);
    return finish();
}
}
