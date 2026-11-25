#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/app/SelectedSoundDocument.h"

#include <algorithm>
#include <cctype>
#include <thread>

// The Sample Editor's Play and Info > Full waveform.
namespace talas::app::workbench
{
namespace
{
// A recording drawn as amplitude against time, its minimum and maximum
// written above it.
class FullWaveformPanel final : public juce::Component
{
public:
    FullWaveformPanel(std::vector<std::vector<double>> channels, talas::app::WaveformExtremes extremes, double seconds)
        : channels_(std::move(channels))
        , extremes_(extremes)
        , seconds_(seconds)
    {
        setTitle("Full waveform");
        setSize(640, 280);
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(panelColour);
        auto bounds = getLocalBounds().reduced(8);
        graphics.setColour(textColour);
        graphics.setFont(WorkbenchType::label());
        graphics.drawText("minimum " + juce::String(extremes_.minimum, 4) + " · maximum " + juce::String(extremes_.maximum, 4)
                + " · " + juce::String(seconds_, 3) + " s",
            bounds.removeFromTop(20), juce::Justification::centredLeft);
        const auto lanes = std::max<std::size_t>(channels_.size(), 1);
        for (std::size_t lane = 0; lane < channels_.size(); ++lane)
        {
            const auto area = bounds.withHeight(bounds.getHeight() / static_cast<int>(lanes))
                                  .withY(bounds.getY() + bounds.getHeight() / static_cast<int>(lanes) * static_cast<int>(lane));
            graphics.setColour(workbenchTheme.controlTile);
            graphics.fillRect(area);
            graphics.setColour(audioColour);
            const auto& data = channels_[lane];
            for (int x = 0; x < area.getWidth() && !data.empty(); ++x)
            {
                const auto first = static_cast<std::size_t>(static_cast<double>(x) / area.getWidth() * static_cast<double>(data.size()));
                const auto last = std::max(first + 1, static_cast<std::size_t>(static_cast<double>(x + 1) / area.getWidth() * static_cast<double>(data.size())));
                auto low = 0.0;
                auto high = 0.0;
                for (auto frame = first; frame < std::min(last, data.size()); ++frame)
                {
                    low = std::min(low, data[frame]);
                    high = std::max(high, data[frame]);
                }
                const auto half = static_cast<double>(area.getHeight()) * 0.5;
                graphics.drawVerticalLine(area.getX() + x, static_cast<float>(area.getCentreY() - high * half),
                    static_cast<float>(area.getCentreY() - low * half + 1.0));
            }
        }
    }

private:
    std::vector<std::vector<double>> channels_;
    talas::app::WaveformExtremes extremes_;
    double seconds_ = 0.0;
};
}

void WorkbenchComponent::playSampleBuffer(std::vector<std::vector<double>> channels, double sampleRate, bool loop)
{
    if (channels.empty() || channels.front().empty() || !(sampleRate > 0.0))
        return;
    auto next = std::make_unique<SampleAudition>();
    for (const auto& channel : channels)
        next->channels.emplace_back(channel.begin(), channel.end());
    auto* device = deviceManager_.getCurrentAudioDevice();
    const auto deviceRate = device != nullptr && device->getCurrentSampleRate() > 0.0 ? device->getCurrentSampleRate() : sampleRate;
    next->step = sampleRate / deviceRate;
    next->loop = loop;
    sampleAuditionPaused_.store(false, std::memory_order_relaxed);
    sampleAuditionPosition_.store(0.0, std::memory_order_relaxed);
    (void) sampleAudition_.exchange(next.get(), std::memory_order_acq_rel);
    while (sampleAuditionReaders_.load(std::memory_order_acquire) != 0)
        std::this_thread::yield();
    sampleAuditionOwned_ = std::move(next);
    setStatus("Playing the sample editor's selection");
}

void WorkbenchComponent::stopSampleBuffer() noexcept
{
    (void) sampleAudition_.exchange(nullptr, std::memory_order_acq_rel);
    while (sampleAuditionReaders_.load(std::memory_order_acquire) != 0)
        std::this_thread::yield();
    sampleAuditionOwned_.reset();
}

// The selected Sound (or the project's output) recorded offline for the time
// asked for, then drawn with its minimum and maximum.
bool WorkbenchComponent::recordFullWaveform(double seconds)
{
    if (!(seconds > 0.0) || seconds > 600.0)
    {
        setStatus("Full waveform records between 0 and 600 seconds");
        return false;
    }
    const auto& document = projectBridge_.editor().document();
    auto played = selectedNodeIds_.size() == 1 && !activeGraphContext_.has_value()
        ? talas::app::documentPlayingSound(document, registry_, selectedNodeIds_.front())
        : std::optional<talas::project::ProjectDocument>{document};
    if (!played.has_value())
        played = document;
    talas::runtime::RenderSettings settings{48000.0, 512};
    settings.variableEnvironment = variableEnvironment_;
    settings.variableLiteralEnvironment = variableLiteralEnvironment_;
    settings.soundVariableEnvironment = soundVariableEnvironment_;
    const auto compiled = talas::runtime::compileProject(*played, registry_, settings);
    if (!compiled.ok())
    {
        setStatus("Full waveform: " + (compiled.diagnostics.empty() ? juce::String{"the Sound did not compile"}
                                                                      : juceString(compiled.diagnostics.front().message)));
        return false;
    }
    const auto offline = talas::runtime::renderOffline(*compiled.graph, static_cast<std::size_t>(seconds * settings.sampleRate));
    if (!offline.ok())
    {
        setStatus("Full waveform: the Sound did not record");
        return false;
    }
    auto channels = talas::app::fullWaveformLanes(offline.mono, offline.right);
    const auto extremes = talas::app::waveformExtremes(channels);
    lastFullWaveformExtremes_ = extremes;
    fullWaveformPanel_ = std::make_unique<FullWaveformPanel>(std::move(channels), extremes, seconds);
    if (isShowing())
    {
        fullWaveformWindow_ = makeToolWindow("Full Waveform", *fullWaveformPanel_);
        fullWaveformWindow_->setVisible(true);
        fullWaveformWindow_->toFront(true);
    }
    setStatus("Full waveform: minimum " + juce::String(extremes.minimum, 4) + " · maximum " + juce::String(extremes.maximum, 4));
    return true;
}

void WorkbenchComponent::showFullWaveform()
{
    // Full waveform takes place within the
    // variables environment, asking first for the values it lacks.
    juce::Component::SafePointer<WorkbenchComponent> asking(this);
    if (askForVariableValues([asking] {
            if (asking != nullptr)
                asking->showFullWaveform();
        }, false))
        return;
    // The length of the sample the Sound plays is
    // suggested.
    const auto sampled = selectedNodeIds_.size() == 1 && !activeGraphContext_.has_value()
        ? talas::app::selectedSoundSampleSeconds(projectBridge_.editor().document(), selectedNodeIds_.front())
        : std::nullopt;
    auto* window = new juce::AlertWindow("Full Waveform", "Record the selected Sound for", juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("seconds", sampled.has_value() ? juce::String{*sampled, 3} : juce::String{"2"}, "Seconds");
    window->addButton("Record", 1, juce::KeyPress{juce::KeyPress::returnKey});
    window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    window->enterModalState(
        true,
        juce::ModalCallbackFunction::create([safeThis, window](int result) {
            const auto seconds = window->getTextEditorContents("seconds").getDoubleValue();
            if (safeThis != nullptr && result == 1)
                (void) safeThis->recordFullWaveform(seconds);
        }),
        true);
}

void WorkbenchComponent::connectDiskSplicerField()
{
    DiskSplicerField::Host host;
    host.samples = [this](std::string_view file) -> std::optional<DiskSplicerField::Channels> {
        const auto& assets = projectBridge_.editor().document().assets;
        const auto asset = std::ranges::find(assets, file, &talas::project::ProjectAsset::id);
        if (asset == assets.end())
            return std::nullopt;
        auto read = talas::audio::readSampleFile(talas::audio::resolveProjectAssetPath(*asset, projectPath()));
        if (!read.ok() || read.channels.empty())
            return std::nullopt;
        return std::move(read.channels);
    };
    // A sample a frame at the rate the signal processor runs at.
    host.sampleRate = [this] {
        auto* device = deviceManager_.getCurrentAudioDevice();
        return device != nullptr && device->getCurrentSampleRate() > 0.0 ? device->getCurrentSampleRate() : 48000.0;
    };
    host.play = [this](DiskSplicerField::Channels channels, double sampleRate) {
        playSampleBuffer(std::move(channels), sampleRate);
        setStatus("Playing the selected segments");
    };
    host.chooseFile = [this](std::function<void(std::string)> chosen) { chooseSplicedFile(std::move(chosen)); };
    inspectorPanel_->setDiskSplicerHost(std::move(host));
}

// The disk button's file list: the sample files the project holds, or
// another file, which the project then holds too.
void WorkbenchComponent::chooseSplicedFile(std::function<void(std::string)> chosen)
{
    const auto isSampleFile = [](const talas::project::ProjectAsset& asset) {
        auto extension = talas::project::pathToUtf8(talas::project::pathFromUtf8(asset.path).extension());
        std::ranges::transform(extension, extension.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return (asset.kind == talas::project::ProjectAssetKind::Table || asset.kind == talas::project::ProjectAssetKind::Audio)
            && (extension == ".wav" || extension == ".aif" || extension == ".aiff" || extension == ".sf" || extension == ".sd1" || extension == ".sd2");
    };
    std::vector<std::string> held;
    for (const auto& asset : projectBridge_.editor().document().assets)
    {
        if (isSampleFile(asset))
            held.push_back(asset.id);
    }
    juce::PopupMenu menu;
    for (std::size_t index = 0; index < held.size(); ++index)
        menu.addItem(static_cast<int>(index + 1), juceString(held[index]));
    if (!held.empty())
        menu.addSeparator();
    const auto other = static_cast<int>(held.size() + 1);
    menu.addItem(other, "Other File...");
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    menu.showMenuAsync(juce::PopupMenu::Options{}, [safeThis, held, other, chosen](int item) {
        if (safeThis == nullptr || item <= 0)
            return;
        if (item < other)
        {
            chosen(held[static_cast<std::size_t>(item - 1)]);
            return;
        }
        safeThis->fileChooser_ = std::make_unique<juce::FileChooser>(
            "Choose a Sample File to Splice", juce::File{}, "*.wav;*.aif;*.aiff;*.sf;*.sd1;*.sd2");
        safeThis->fileChooser_->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [safeThis, chosen](const juce::FileChooser& chooser) {
                if (safeThis == nullptr || chooser.getResult() == juce::File{})
                    return;
                if (const auto file = safeThis->holdSampleFile(
                        talas::project::pathFromUtf8(chooser.getResult().getFullPathName().toStdString())))
                    chosen(*file);
            });
    });
}

// The project holds the file, copied into its package when it is one: the
// name it holds it under.
std::optional<std::string> WorkbenchComponent::holdSampleFile(const std::filesystem::path& file)
{
    auto asset = makeAssetReference(file);
    std::optional<std::filesystem::path> importedPath;
    if (!importAssetIntoPackage(asset, file, importedPath))
        return std::nullopt;
    if (const auto added = projectBridge_.editor().addAsset(asset); !added.ok())
    {
        if (importedPath.has_value())
        {
            std::error_code error;
            std::filesystem::remove(*importedPath, error);
        }
        setStatus("The file could not be added: " + juceString(added.message));
        return std::nullopt;
    }
    documentChanged();
    return asset.id;
}
}
