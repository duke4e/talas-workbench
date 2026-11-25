#include "app/workbench/WorkbenchAlternateTunings.h"

#include "app/workbench/WorkbenchTheme.h"
#include "talas/project/ProjectPathEncoding.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>

namespace talas::app::workbench
{
namespace
{
namespace tunings = talas::project::tunings;

[[nodiscard]] juce::String numberText(double value)
{
    std::array<char, 32> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%.10g", value);
    return juce::String{buffer.data()};
}

[[nodiscard]] std::optional<double> numberIn(const juce::String& text)
{
    const auto source = text.trim().toStdString();
    char* end = nullptr;
    const auto value = std::strtod(source.c_str(), &end);
    if (source.empty() || end != source.c_str() + source.size() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

class AlternateTuningsPanelImpl final : public AlternateTuningsPanel
{
public:
    AlternateTuningsPanelImpl(StatusCallback onStatus, ExampleCallback onExample)
        : onStatus_(std::move(onStatus))
        , onExample_(std::move(onExample))
    {
        setTitle("Design Alternate Tunings");
        // The main page's buttons for the five kinds of tuning, each showing
        // its page, and Back to the main page.
        for (std::size_t kind = 0; kind < kinds_.size(); ++kind)
        {
            kinds_[kind].setButtonText(kindNames[kind]);
            kinds_[kind].onClick = [this, kind] {
                tuning_.method = static_cast<tunings::Method>(kind);
                page_ = tuning_.method;
                showFields();
                update();
            };
            addAndMakeVisible(kinds_[kind]);
        }
        back_.onClick = [this] {
            page_.reset();
            showFields();
        };
        // A 21-harmonic sawtooth, a sine or a sample.
        for (auto* button : {&sawtooth_, &sine_, &sampled_})
        {
            button->setRadioGroupId(1);
            button->setClickingTogglesState(true);
            button->onClick = [this] { tryOut(); };
            addAndMakeVisible(*button);
        }
        sawtooth_.setToggleState(true, juce::dontSendNotification);
        // The example scales.
        for (auto& [button, example] : presets_)
        {
            button->onClick = [this, name = example] { (void)preset(name); };
            addAndMakeVisible(*button);
        }
        const auto field = [this](juce::TextEditor& editor, const juce::String& title) {
            editor.setTitle(title);
            editor.setFont(WorkbenchType::body());
            editor.onReturnKey = [this, &editor] { (void)enter(editor); };
            editor.onFocusLost = [this, &editor] { (void)enter(editor); };
            addAndMakeVisible(editor);
        };
        field(tonicKey_, "Tonic Key Number");
        field(tonicFrequency_, "Tonic Frequency");
        field(degrees_, "Scale degrees per octave");
        field(interval_, "Generating interval");
        for (std::size_t degree = 0; degree < tunings::scaleDegrees; ++degree)
        {
            const auto name = juce::String{static_cast<int>(degree) + 1};
            field(numerators_[degree], "Numerator " + name);
            field(denominators_[degree], "Denominator " + name);
            field(cents_[degree], "Cents " + name);
        }
        for (const auto& [label, text] : std::initializer_list<std::pair<juce::Label*, const char*>>{{&tonicKeyLabel_, "Tonic Key Number"},
                 {&tonicFrequencyLabel_, "Tonic Frequency (hz)"}, {&methodLabel_, ""},
                 {&degreesLabel_, "Scale degrees per octave"}, {&intervalLabel_, "Generating interval"},
                 {&numeratorsLabel_, "Numerators"}, {&denominatorsLabel_, "Denominators"}, {&centsLabel_, "Cents"},
                 {&fileLabel_, "All pitches come from the file: Load... a tuning file"},
                 {&instrumentLabel_, "Example instrument"}, {&sampleLabel_, "No sample chosen"}})
        {
            label->setText(text, juce::dontSendNotification);
            label->setColour(juce::Label::textColourId, secondaryTextColour);
            addAndMakeVisible(*label);
        }
        octave_.setMultiLine(true, false);
        octave_.setReadOnly(true);
        octave_.setFont(WorkbenchType::data());
        octave_.setTitle("The octave from the tonic");
        choose_.onClick = [this] {
            chooser_ = std::make_unique<juce::FileChooser>("Choose the Example's Sample", juce::File{}, "*.wav;*.aif;*.aiff;*.sf;*.sd1;*.sd2");
            chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                [this](const juce::FileChooser& chooser) {
                    if (chooser.getResult() == juce::File{})
                        return;
                    sample_ = talas::project::pathFromUtf8(chooser.getResult().getFullPathName().toStdString());
                    sampleLabel_.setText(chooser.getResult().getFileName(), juce::dontSendNotification);
                    sampled_.setToggleState(true, juce::dontSendNotification);
                    tryOut();
                });
        };
        load_.onClick = [this] {
            chooser_ = std::make_unique<juce::FileChooser>("Load a Tuning", juce::File{}, "*.txt");
            chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                [this](const juce::FileChooser& chooser) {
                    if (chooser.getResult() != juce::File{})
                        (void)load(talas::project::pathFromUtf8(chooser.getResult().getFullPathName().toStdString()));
                });
        };
        save_.onClick = [this] {
            chooser_ = std::make_unique<juce::FileChooser>("Save the Tuning", juce::File{}, "*.txt");
            chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                    | juce::FileBrowserComponent::warnAboutOverwriting,
                [this](const juce::FileChooser& chooser) {
                    if (chooser.getResult() != juce::File{})
                    {
                        (void)save(talas::project::pathFromUtf8(
                            chooser.getResult().withFileExtension("txt").getFullPathName().toStdString()));
                    }
                });
        };
        example_.onClick = [this] { (void)createExample(); };
        for (auto* component : std::initializer_list<juce::Component*>{
                 &octave_, &choose_, &load_, &save_, &example_, &back_})
        {
            addAndMakeVisible(*component);
        }
        refreshFields();
        showFields();
        setSize(760, 560);
        update();
    }

    [[nodiscard]] tunings::Tuning tuningForSmoke() const override
    {
        return tuning_;
    }

    void setTuningForSmoke(const tunings::Tuning& tuning) override
    {
        tuning_ = tuning;
        page_ = tuning_.method;
        refreshFields();
        showFields();
        update();
    }

    [[nodiscard]] bool enterForSmoke(const juce::String& field, const juce::String& value) override
    {
        auto* editor = fieldNamed(field);
        if (editor == nullptr)
            return false;
        editor->setText(value, false);
        return enter(*editor);
    }

    [[nodiscard]] bool leaveFieldForSmoke(const juce::String& field, const juce::String& value) override
    {
        auto* editor = fieldNamed(field);
        if (editor == nullptr || !editor->onFocusLost)
            return false;
        editor->setText(value, false);
        editor->onFocusLost();
        return true;
    }

    [[nodiscard]] juce::String octaveTextForSmoke() const override
    {
        return octave_.getText();
    }

    [[nodiscard]] bool saveForSmoke(const std::filesystem::path& file) override
    {
        return save(file);
    }

    [[nodiscard]] bool loadForSmoke(const std::filesystem::path& file) override
    {
        return load(file);
    }

    [[nodiscard]] bool createExampleForSmoke() override
    {
        return createExample();
    }

    void chooseInstrumentForSmoke(talas::app::TuningExampleInstrument instrument) override
    {
        (instrument == talas::app::TuningExampleInstrument::Sawtooth ? sawtooth_
                : instrument == talas::app::TuningExampleInstrument::Sine ? sine_
                                                                       : sampled_)
            .setToggleState(true, juce::dontSendNotification);
        tryOut();
    }

    [[nodiscard]] bool presetForSmoke(const juce::String& example) override
    {
        return preset(example);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(8);
        const auto row = [&](int height) {
            auto taken = bounds.removeFromTop(height);
            bounds.removeFromTop(4);
            return taken;
        };
        if (!page_.has_value())
        {
            // The main page: the tonic, the example's instrument, the kinds.
            auto settings = row(24);
            tonicKeyLabel_.setBounds(settings.removeFromLeft(120));
            tonicKey_.setBounds(settings.removeFromLeft(50));
            settings.removeFromLeft(12);
            tonicFrequencyLabel_.setBounds(settings.removeFromLeft(140));
            tonicFrequency_.setBounds(settings.removeFromLeft(100));
            bounds.removeFromTop(12);
            auto instrument = row(24);
            instrumentLabel_.setBounds(instrument.removeFromLeft(130));
            for (auto* button : {&sawtooth_, &sine_, &sampled_})
                button->setBounds(instrument.removeFromLeft(button == &sawtooth_ ? 130 : 70));
            instrument.removeFromLeft(6);
            choose_.setBounds(instrument.removeFromLeft(90));
            instrument.removeFromLeft(6);
            sampleLabel_.setBounds(instrument);
            bounds.removeFromTop(12);
            for (auto& kind : kinds_)
                kind.setBounds(row(28).removeFromLeft(180));
            return;
        }
        // A kind's page: its fields, the octave, the presets at the lower
        // left and the page's buttons at the lower right.
        methodLabel_.setBounds(row(24));
        const auto degreeRow = [&](juce::Label& label, std::array<juce::TextEditor, tunings::scaleDegrees>& editors) {
            if (!label.isVisible())
                return;
            auto line = row(24);
            label.setBounds(line.removeFromLeft(100));
            const auto width = line.getWidth() / static_cast<int>(tunings::scaleDegrees);
            for (auto& editor : editors)
                editor.setBounds(line.removeFromLeft(width).reduced(1, 0));
        };
        degreeRow(numeratorsLabel_, numerators_);
        degreeRow(denominatorsLabel_, denominators_);
        degreeRow(centsLabel_, cents_);
        if (degreesLabel_.isVisible())
        {
            auto parameters = row(24);
            degreesLabel_.setBounds(parameters.removeFromLeft(170));
            degrees_.setBounds(parameters.removeFromLeft(60));
            parameters.removeFromLeft(12);
            intervalLabel_.setBounds(parameters.removeFromLeft(130));
            interval_.setBounds(parameters.removeFromLeft(90));
        }
        if (fileLabel_.isVisible())
            fileLabel_.setBounds(row(24));
        auto buttons = bounds.removeFromBottom(28);
        for (auto& [button, example] : presets_)
        {
            if (button->isVisible())
                button->setBounds(buttons.removeFromLeft(110).reduced(2, 0));
        }
        for (auto* button : {&back_, &save_, &load_})
        {
            button->setBounds(buttons.removeFromRight(80));
            buttons.removeFromRight(6);
        }
        example_.setBounds(buttons.removeFromRight(120));
        bounds.removeFromBottom(6);
        octave_.setBounds(bounds);
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(panelColour);
    }

private:
    [[nodiscard]] juce::TextEditor* fieldNamed(const juce::String& name)
    {
        if (name == "tonicKey")
            return &tonicKey_;
        if (name == "tonicFrequency")
            return &tonicFrequency_;
        if (name == "degrees")
            return &degrees_;
        if (name == "interval")
            return &interval_;
        for (const auto& [prefix, editors] : {std::pair{juce::String{"numerator"}, &numerators_},
                 std::pair{juce::String{"denominator"}, &denominators_}, std::pair{juce::String{"cents"}, &cents_}})
        {
            if (!name.startsWith(prefix))
                continue;
            const auto index = name.substring(prefix.length()).getIntValue();
            if (index >= 1 && index <= static_cast<int>(tunings::scaleDegrees))
                return &(*editors)[static_cast<std::size_t>(index) - 1];
        }
        return nullptr;
    }

    // Takes a field's value into the tuning, as pressing Enter, Tab or
    // clicking another field does; a value the tuning refuses
    // returns the field to the tuning's.
    bool enter(juce::TextEditor& editor)
    {
        auto candidate = tuning_;
        const auto value = numberIn(editor.getText());
        auto taken = value.has_value();
        if (taken)
        {
            const auto whole = *value == std::floor(*value);
            if (&editor == &tonicKey_)
                taken = whole && (candidate.tonicKey = static_cast<int>(*value), true);
            else if (&editor == &tonicFrequency_)
                candidate.tonicFrequency = *value;
            else if (&editor == &degrees_)
                taken = whole && (candidate.degreesPerOctave = static_cast<int>(*value), true);
            else if (&editor == &interval_)
                candidate.generatingInterval = *value;
            for (std::size_t degree = 0; degree < tunings::scaleDegrees; ++degree)
            {
                if (&editor == &numerators_[degree])
                    candidate.ratios[degree].numerator = *value;
                if (&editor == &denominators_[degree])
                    candidate.ratios[degree].denominator = *value;
                if (&editor == &cents_[degree])
                    candidate.cents[degree] = *value;
            }
        }
        std::string error;
        if (!taken)
            error = "The field takes a number";
        else if (!tunings::keyFrequencies(candidate, error))
            taken = false;
        if (!taken)
        {
            onStatus_(juce::String{error});
            refreshFields();
            return false;
        }
        if (candidate == tuning_)
            return true;
        tuning_ = candidate;
        update();
        return true;
    }

    void refreshFields()
    {
        tonicKey_.setText(juce::String{tuning_.tonicKey}, false);
        tonicFrequency_.setText(numberText(tuning_.tonicFrequency), false);
        degrees_.setText(juce::String{tuning_.degreesPerOctave}, false);
        interval_.setText(numberText(tuning_.generatingInterval), false);
        for (std::size_t degree = 0; degree < tunings::scaleDegrees; ++degree)
        {
            numerators_[degree].setText(numberText(tuning_.ratios[degree].numerator), false);
            denominators_[degree].setText(numberText(tuning_.ratios[degree].denominator), false);
            cents_[degree].setText(numberText(tuning_.cents[degree]), false);
        }
    }

    // Only the current method's parameters show.
    void showFields()
    {
        using Method = tunings::Method;
        using Part = talas::app::TuningToolPart;
        const auto shows = [this](Part part) { return talas::app::tuningToolPageShows(page_, part); };
        for (auto* component : std::initializer_list<juce::Component*>{
                 &tonicKey_, &tonicKeyLabel_, &tonicFrequency_, &tonicFrequencyLabel_})
            component->setVisible(shows(Part::Tonic));
        for (auto* component : std::initializer_list<juce::Component*>{
                 &instrumentLabel_, &sawtooth_, &sine_, &sampled_, &choose_, &sampleLabel_})
            component->setVisible(shows(Part::Instrument));
        for (auto& kind : kinds_)
            kind.setVisible(shows(Part::KindButtons));
        octave_.setVisible(shows(Part::Octave));
        for (auto* component : std::initializer_list<juce::Component*>{&example_, &load_, &save_, &back_})
            component->setVisible(shows(Part::PageButtons));
        // The page's own fields: none on the main page.
        const auto method = shows(Part::KindFields) ? std::optional{tuning_.method} : std::nullopt;
        methodLabel_.setVisible(method.has_value());
        if (method.has_value())
            methodLabel_.setText(kindNames[static_cast<std::size_t>(*method)], juce::dontSendNotification);
        for (std::size_t degree = 0; degree < tunings::scaleDegrees; ++degree)
        {
            numerators_[degree].setVisible(method == Method::RatioScale);
            denominators_[degree].setVisible(method == Method::RatioScale);
            cents_[degree].setVisible(method == Method::CentsScale);
        }
        numeratorsLabel_.setVisible(method == Method::RatioScale);
        denominatorsLabel_.setVisible(method == Method::RatioScale);
        centsLabel_.setVisible(method == Method::CentsScale);
        const auto degrees = method == Method::EqualTempered || method == Method::TwoInterval;
        degrees_.setVisible(degrees);
        degreesLabel_.setVisible(degrees);
        interval_.setVisible(method == Method::TwoInterval);
        intervalLabel_.setVisible(method == Method::TwoInterval);
        fileLabel_.setVisible(method == Method::FromTextFile);
        for (auto& [button, example] : presets_)
        {
            const auto cents = example == "Meantone" || example == "Werckmeister";
            button->setVisible(example == "Equal" ? method == Method::RatioScale || method == Method::CentsScale
                    : cents                      ? method == Method::CentsScale
                                                 : method == Method::RatioScale);
        }
        resized();
    }

    // An example scale's ratios, or cents, in place of the fields'.
    bool preset(const juce::String& example)
    {
        using Method = tunings::Method;
        auto candidate = tuning_;
        if (tuning_.method == Method::RatioScale && (example == "Equal" || example == "Just" || example == "Pythagorean"))
            candidate.ratios = tunings::exampleRatios(example == "Equal" ? tunings::RatioExample::EqualTempered
                    : example == "Just"                                ? tunings::RatioExample::Just
                                                                       : tunings::RatioExample::Pythagorean);
        else if (tuning_.method == Method::CentsScale && (example == "Equal" || example == "Meantone" || example == "Werckmeister"))
            candidate.cents = tunings::exampleCents(example == "Equal" ? tunings::CentsExample::EqualTempered
                    : example == "Meantone"                           ? tunings::CentsExample::Meantone
                                                                      : tunings::CentsExample::WerckmeisterIII);
        else
            return false;
        tuning_ = candidate;
        refreshFields();
        update();
        onStatus_("The " + example + " scale");
        return true;
    }

    // The octave from the tonic, as the central view lists it.
    void update()
    {
        std::string error;
        frequencies_ = tunings::keyFrequencies(tuning_, error);
        if (!frequencies_)
        {
            octave_.setText(juce::String{error}, false);
            return;
        }
        juce::String text = "Key   Frequency       Ratio          Cents\n";
        for (const auto& degree : tunings::octaveFromTonic(tuning_, *frequencies_))
        {
            std::array<char, 128> line{};
            std::snprintf(line.data(), line.size(), "%3d   %10.4f hz   %10.6f   %9.3f\n", degree.key, degree.frequency,
                degree.ratio, degree.cents);
            text += line.data();
        }
        octave_.setText(text, false);
        tryOut();
    }

    void setTryOut(TryOutCallback tryOut) override
    {
        tryOut_ = std::move(tryOut);
        this->tryOut();
    }

    [[nodiscard]] talas::app::TuningExampleInstrument instrument() const
    {
        return sampled_.getToggleState() ? talas::app::TuningExampleInstrument::Sample
            : sine_.getToggleState()     ? talas::app::TuningExampleInstrument::Sine
                                         : talas::app::TuningExampleInstrument::Sawtooth;
    }

    // The keyboard plays the tuning throughout; a sample
    // instrument waits for its sample.
    void tryOut()
    {
        if (tryOut_ && frequencies_ && !(instrument() == talas::app::TuningExampleInstrument::Sample && sample_.empty()))
            tryOut_(tuning_, *frequencies_, instrument(), sample_);
    }

    bool save(const std::filesystem::path& file)
    {
        if (!frequencies_)
            return false;
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output << tunings::tuningText(tuning_, *frequencies_);
        if (!output.good())
        {
            onStatus_("The tuning could not be saved");
            return false;
        }
        onStatus_("Saved the tuning's 128 keys to " + juce::String{talas::project::pathToUtf8(file.filename())});
        return true;
    }

    bool load(const std::filesystem::path& file)
    {
        std::ifstream input(file, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        std::string error;
        const auto loaded = tunings::readTuningText(text, tuning_.method, tuning_.tonicKey, error);
        if (!input.good() && !input.eof())
            error = "The tuning file could not be read";
        if (!loaded)
        {
            onStatus_(juce::String{error});
            return false;
        }
        tuning_ = *loaded;
        refreshFields();
        update();
        onStatus_("Loaded the tuning from " + juce::String{talas::project::pathToUtf8(file.filename())});
        return true;
    }

    bool createExample()
    {
        if (!frequencies_)
            return false;
        const auto instrument = this->instrument();
        if (instrument == talas::app::TuningExampleInstrument::Sample && sample_.empty())
        {
            onStatus_("Choose... a sample for the example first");
            return false;
        }
        return onExample_(tuning_, *frequencies_, instrument, sample_);
    }

    StatusCallback onStatus_;
    ExampleCallback onExample_;
    TryOutCallback tryOut_;
    tunings::Tuning tuning_;
    std::optional<tunings::KeyFrequencies> frequencies_;
    std::filesystem::path sample_;
    juce::TextEditor tonicKey_;
    juce::TextEditor tonicFrequency_;
    juce::TextEditor degrees_;
    juce::TextEditor interval_;
    std::array<juce::TextEditor, tunings::scaleDegrees> numerators_;
    std::array<juce::TextEditor, tunings::scaleDegrees> denominators_;
    std::array<juce::TextEditor, tunings::scaleDegrees> cents_;
    juce::Label tonicKeyLabel_;
    juce::Label tonicFrequencyLabel_;
    juce::Label methodLabel_;
    juce::Label degreesLabel_;
    juce::Label intervalLabel_;
    juce::Label numeratorsLabel_;
    juce::Label denominatorsLabel_;
    juce::Label centsLabel_;
    juce::Label fileLabel_;
    juce::Label instrumentLabel_;
    juce::Label sampleLabel_;
    static constexpr std::array<const char*, 5> kindNames{
        "Ratio Scale", "Cents Scale", "Equal Tempered", "Two Interval", "From Text File"};
    std::array<juce::TextButton, 5> kinds_;
    juce::TextButton back_{"Back"};
    // The kind of tuning whose page is shown, or none for the main page.
    std::optional<tunings::Method> page_;
    juce::ToggleButton sawtooth_{"21-harmonic sawtooth"};
    juce::ToggleButton sine_{"Sine"};
    juce::ToggleButton sampled_{"Sample"};
    std::array<std::pair<std::unique_ptr<juce::TextButton>, juce::String>, 5> presets_{{
        {std::make_unique<juce::TextButton>("Equal"), "Equal"},
        {std::make_unique<juce::TextButton>("Just"), "Just"},
        {std::make_unique<juce::TextButton>("Pythagorean"), "Pythagorean"},
        {std::make_unique<juce::TextButton>("Meantone"), "Meantone"},
        {std::make_unique<juce::TextButton>("Werckmeister III"), "Werckmeister"},
    }};
    juce::TextEditor octave_;
    juce::TextButton choose_{"Choose..."};
    juce::TextButton load_{"Load..."};
    juce::TextButton save_{"Save..."};
    juce::TextButton example_{"Create Example"};
    std::unique_ptr<juce::FileChooser> chooser_;
};
}

std::unique_ptr<AlternateTuningsPanel> makeAlternateTuningsPanel(
    AlternateTuningsPanel::StatusCallback onStatus, AlternateTuningsPanel::ExampleCallback onExample)
{
    return std::make_unique<AlternateTuningsPanelImpl>(std::move(onStatus), std::move(onExample));
}
}
