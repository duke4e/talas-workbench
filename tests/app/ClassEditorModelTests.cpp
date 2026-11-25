#include "talas/app/ClassEditorModel.h"
#include "talas/app/PrototypeBrowserModel.h"
#include "talas/app/SoundFileWindowModel.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/expr/Expression.h"
#include "talas/project/ProjectClassFromExample.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectValidation.h"
#include "talas/project/ProjectVariables.h"

#include <algorithm>
#include <optional>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::ProjectSoundExposedParameter frequency()
{
    talas::project::ProjectSoundExposedParameter parameter;
    parameter.parameterId = "freq";
    parameter.displayName = "Frequency";
    parameter.domain = talas::expr::ValueDomain::Hertz;
    parameter.defaultValue.parameterId = "freq";
    parameter.defaultValue.expression = "440 hz";
    parameter.bindings.push_back({talas::project::ProjectSoundParameterTarget::NodeParameter, "osc", "frequency", {}, {}});
    return parameter;
}

talas::project::ProjectSoundExposedParameter amplitude()
{
    talas::project::ProjectSoundExposedParameter parameter;
    parameter.parameterId = "amp";
    parameter.displayName = "Amplitude";
    parameter.defaultValue.parameterId = "amp";
    parameter.defaultValue.expression = "0.5";
    parameter.bindings.push_back({talas::project::ProjectSoundParameterTarget::NodeParameter, "osc", "amplitude", {},
        talas::expr::ValueDomain::Scalar});
    return parameter;
}

talas::project::ProjectDocument voiceDocument()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition voice;
    voice.definitionId = "voice";
    voice.displayName = "Voice";
    voice.graph.nodes.push_back({"osc", "talas.oscillator", {}});
    voice.outputs.push_back({"audioOut", talas::core::PortType::Audio, "osc", "audioOut"});
    voice.exposedParameters = {frequency(), amplitude()};
    document.soundDefinitions.push_back(std::move(voice));
    return document;
}

// The Field Rectangle, `upperLeftX @ upperLeftY corner:
// lowerRightX @ lowerRightY`, within 1 @ 1.
void testFieldRectangles()
{
    const auto rectangle = talas::app::parseFieldRectangle("0 @ 0.5 corner: 1@1");
    require(rectangle == std::optional<talas::project::ProjectSoundFieldRectangle>{{0.0, 0.5, 1.0, 1.0}},
        "a Field Rectangle should read its corners");
    require(talas::app::fieldRectangleText(*rectangle) == "0 @ 0.5 corner: 1 @ 1", "a rectangle should write as x @ y corner: x @ y");
    // The tutorial's field, its upper left corner in the middle and
    // flush with the top, its lower right flush with the right edge.
    require(talas::app::parseFieldRectangle("0.5 @ 0 corner: 1 @ 0.8")
            == std::optional<talas::project::ProjectSoundFieldRectangle>{{0.5, 0.0, 1.0, 0.8}},
        "the tutorial's rectangle should start in the middle at the top and reach the right edge");
    for (const auto* bad : {"0 @ 0 corner: 2 @ 1", "0.5 @ 0 corner: 0.25 @ 1", "0 0 1 1", "0 @ x corner: 1 @ 1"})
        require(!talas::app::parseFieldRectangle(bad).has_value(), std::string{"a rectangle should be refused: "} + bad);

    // The Field Layout shows each field where its
    // rectangle puts it, the selected parameter's highlighted.
    auto voice = voiceDocument().soundDefinitions.front();
    voice.exposedParameters[0].field = talas::project::ProjectSoundFieldRectangle{0.0, 0.0, 0.5, 0.8};
    voice.exposedParameters[1].field = talas::project::ProjectSoundFieldRectangle{0.5, 0.0, 1.0, 0.8};
    const auto layout = talas::app::classEditorFieldLayout(voice, "amp");
    require(layout.size() == 2 && layout[0].name == "Frequency" && !layout[0].highlighted && layout[1].name == "Amplitude"
            && layout[1].highlighted && layout[1].field == talas::project::ProjectSoundFieldRectangle{0.5, 0.0, 1.0, 0.8},
        "the selected parameter's field should be highlighted where its rectangle lies");
    require(talas::app::classEditorFieldLayout(voice, "freq")[0].highlighted && !talas::app::classEditorFieldLayout(voice, "freq")[1].highlighted,
        "selecting another parameter should move the highlight");
    voice.exposedParameters[0].field.reset();
    require(talas::app::classEditorFieldLayout(voice, "freq").size() == 1, "a parameter with no rectangle should have no place drawn");

    // The tutorial's fields side by side in thirds,
    // the input below at the left and amps beside it to the right edge,
    // touching but not overlapping; one moved over another is marked.
    talas::project::ProjectSoundDefinition tutorial;
    for (const auto& [name, rectangle] : std::vector<std::pair<std::string, std::string>>{
             {"input", "0 @ 0.5 corner: 0.3333 @ 1"}, {"freq", "0 @ 0 corner: 0.3333 @ 0.3333"},
             {"octSpacing", "0.3333 @ 0 corner: 0.6667 @ 0.3333"}, {"NbrFilts", "0.6667 @ 0 corner: 1 @ 0.3333"},
             {"amps", "0.3333 @ 0.5 corner: 1 @ 1"}})
    {
        talas::project::ProjectSoundExposedParameter parameter;
        parameter.parameterId = name;
        parameter.displayName = name;
        parameter.field = talas::app::parseFieldRectangle(rectangle);
        require(parameter.field.has_value(), "the tutorial's rectangle should read: " + rectangle);
        tutorial.exposedParameters.push_back(parameter);
    }
    const auto thirds = talas::app::classEditorFieldLayout(tutorial, "amps");
    require(thirds.size() == 5 && std::ranges::none_of(thirds, &talas::app::ClassEditorLayoutBox::overlaps) && thirds[4].highlighted,
        "fields side by side should not overlap");
    tutorial.exposedParameters[4].field = talas::app::parseFieldRectangle("0.2 @ 0.5 corner: 1 @ 1");
    const auto overlapping = talas::app::classEditorFieldLayout(tutorial, "amps");
    require(overlapping[0].overlaps && overlapping[4].overlaps && !overlapping[1].overlaps,
        "a field moved over another should be marked as overlapping it");
}

// A parameter's type, comment and range.
void testParameterFields()
{
    require(talas::app::soundParameterHelp(frequency()) == "No help is available.",
        "a parameter with no comment and no range should say that no help is available");
    auto fields = talas::app::classEditorFields(frequency());
    require(fields.displayName == "Frequency" && fields.domain == "hertz" && fields.minimum.empty() && fields.field.empty(),
        "the editor should show a parameter's name and type");
    fields.minimum = "20";
    fields.maximum = "20000";
    fields.field = "0 @ 0 corner: 1 @ 0.5";
    fields.comment = "The fundamental of every partial.";
    std::string error;
    const auto edited = talas::app::applyClassEditorFields(frequency(), fields, error);
    require(edited.has_value() && edited->minimum == std::optional<double>{20.0} && edited->maximum == std::optional<double>{20000.0}
            && edited->field.has_value() && edited->comment == "The fundamental of every partial.",
        "the editor should set the range, field and comment: " + error);
    require(talas::app::soundParameterHelp(*edited) == "The fundamental of every partial.\nRange 20 to 20000",
        "an instance's field should show the comment and the range");
    auto bad = fields;
    bad.maximum = "10";
    require(!talas::app::applyClassEditorFields(frequency(), bad, error).has_value(), "a maximum below the minimum");
    bad = fields;
    bad.domain = "volts";
    require(!talas::app::applyClassEditorFields(frequency(), bad, error).has_value(), "an unknown type");
}

// The fields are the Sound's own, saved in format 99; an instance's value
// outside the range is refused, and the Field Layout orders the
// instance's fields.
// Closing the editor asks about
// changes not yet kept, and only then.
void testClosingAsksAboutChangesNotKept()
{
    talas::project::ProjectSoundExposedParameter parameter;
    parameter.parameterId = "cutoff";
    parameter.displayName = "Cutoff";
    parameter.domain = talas::expr::ValueDomain::Hertz;
    auto fields = talas::app::classEditorFields(parameter);
    require(!talas::app::classEditorFieldsChanged(parameter, fields), "the fields as shown are no change");
    fields.comment = "The filter's cutoff";
    require(talas::app::classEditorFieldsChanged(parameter, fields), "a comment written is a change to save or drop");
    fields = talas::app::classEditorFields(parameter);
    fields.field = "not a rectangle";
    require(talas::app::classEditorFieldsChanged(parameter, fields), "a field that does not read is a change not kept");
}

void testProjectClassFields()
{
    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    talas::project::ProjectEditor editor(voiceDocument(), registry);
    auto voice = editor.document().soundDefinitions.front();
    voice.description = "A single partial.";
    voice.exposedParameters[0].minimum = 20.0;
    voice.exposedParameters[0].maximum = 2000.0;
    voice.exposedParameters[0].field = talas::project::ProjectSoundFieldRectangle{0.0, 0.5, 1.0, 1.0};
    voice.exposedParameters[0].comment = "Pitch";
    voice.exposedParameters[1].field = talas::project::ProjectSoundFieldRectangle{0.0, 0.0, 1.0, 0.5};
    const auto replaced = editor.replaceSoundDefinition("voice", voice);
    require(replaced.ok(), "the Class Editor's fields should be accepted: " + replaced.message);
    require(editor.addSoundInstance("voice1", "voice", {}).ok(), "an instance of the voice");
    talas::project::ProjectParameterExpression low;
    low.parameterId = "freq";
    low.expression = "10 hz";
    require(!editor.setParameter("voice1", low).ok(), "a value below the range should be refused");
    low.expression = "100 hz";
    require(editor.setParameter("voice1", low).ok(), "a value within the range should be accepted");
    low.expression = "!Pitch";
    require(editor.setParameter("voice1", low).ok(), "an Event Value is checked only as it plays");
    const auto descriptor = talas::project::describeProjectNode(
        editor.document(), editor.document().graph.nodes.front(), registry);
    require(descriptor.has_value() && descriptor->parameters.size() == 2 && descriptor->parameters[0].id == "amp",
        "the field at the top should come first");
    auto outOfRange = voice;
    outOfRange.exposedParameters[0].minimum = 500.0;
    require(!editor.replaceSoundDefinition("voice", outOfRange).ok(), "a default outside its range should be refused");
    const auto saved = talas::project::serializeProjectDocument(editor.document());
    require(saved.find("\"description\": \"A single partial.\"") != std::string::npos
            && saved.find("\"comment\": \"Pitch\"") != std::string::npos && saved.find("\"minimum\": 20") != std::string::npos,
        "format 99 should save the Class Editor's fields");
    const auto loaded = talas::project::parseProjectDocument(saved);
    require(loaded.ok()
            && loaded.document.soundDefinitions.front().exposedParameters
                == editor.document().soundDefinitions.front().exposedParameters
            && loaded.document.soundDefinitions.front().exposedParameters[0].field == voice.exposedParameters[0].field
            && loaded.document.soundDefinitions.front().description == "A single partial.",
        "the fields should load as saved: " + (loaded.errors.empty() ? std::string{} : loaded.errors.front().message));
    auto older = saved;
    older.replace(older.find("\"version\": 156"), 13, "\"version\": 98");
    require(!talas::project::parseProjectDocument(older).ok(), "format 98 should refuse the Class Editor's fields");
}
}

// The Field list holds
// the kinds of field a parameter's kind can have, and a type selects its
// default; a value field takes constants, variables and expressions but no
// Event Values, a hotValue field those too, and a value field's constant
// expression is evaluated as it is entered; a codeString field's Smalltalk
// is parsed as it is entered, a syntax error told by line and column; a
// string field takes its text as typed, never a variable; radioButtons
// fields show a choice's values in one column or two.
void testParameterFieldKinds()
{
    using Kind = talas::core::ParameterKind;
    using Field = talas::project::ProjectSoundFieldKind;
    require(talas::app::classEditorFieldKinds(Kind::Expression) == std::vector<std::string_view>{"hotValue", "value"}
            && talas::app::classEditorFieldKinds(Kind::Text)
                == std::vector<std::string_view>{"string", "codeString", "complex", "polesAndZeroes", "grammar"}
            && talas::app::classEditorFieldKinds(Kind::Choice)
                == std::vector<std::string_view>{"symbolList", "radioButtons", "radioButtonsTwoColumns"}
            && talas::app::classEditorFieldKinds(Kind::Boolean) == std::vector<std::string_view>{"boolean"},
        "the Field list should hold the fields of the parameter's kind, its default first");
    require(talas::app::classEditorFieldKindName(frequency()) == "hotValue",
        "a parameter given no field should have its kind's default, a hotValue field");

    // A type selects its default field, hotValue for a
    // hot type.
    std::string error;
    const auto cold = talas::app::applyTalasParameterType(frequency(), *talas::app::findTalasParameterType("frequency"), error);
    const auto hot = talas::app::applyTalasParameterType(frequency(), *talas::app::findTalasParameterType("hotFrequency"), error);
    const auto anything = talas::app::applyTalasParameterType(amplitude(), *talas::app::findTalasParameterType("hotValue"), error);
    require(cold && cold->fieldKind == Field::Value && talas::app::classEditorFields(*cold).fieldKind == "value" && hot
            && hot->fieldKind == Field::HotValue && anything && anything->fieldKind == Field::HotValue
            && talas::app::talasParameterTypeField(*talas::app::findTalasParameterType("hotFrequency")) == "hotValue",
        "a type should select its default field, the hotValue type a hotValue field: " + error);
    auto fields = talas::app::classEditorFields(frequency());
    require(!talas::app::classEditorFieldsChanged(frequency(), fields), "the field shown should be no change");
    fields.fieldKind = "value";
    const auto valued = talas::app::applyClassEditorFields(frequency(), fields, error);
    require(valued && valued->fieldKind == Field::Value && talas::app::classEditorFieldsChanged(frequency(), fields),
        "the value field chosen should be kept: " + error);
    fields.fieldKind = "radioButtons";
    require(!talas::app::applyClassEditorFields(frequency(), fields, error) && error.find("hotValue or value") != std::string::npos,
        "a field of another kind should be refused: " + error);

    // Radio buttons in one column, or two filled first on the left.
    using Place = talas::app::RadioButtonPlace;
    require(talas::app::radioButtonPlaces(3, 1) == std::vector<Place>{{0, 0}, {0, 1}, {0, 2}}
            && talas::app::radioButtonPlaces(3, 2) == std::vector<Place>{{0, 0}, {0, 1}, {1, 0}}
            && talas::app::radioButtonPlaces(4, 2) == std::vector<Place>{{0, 0}, {0, 1}, {1, 0}, {1, 1}},
        "radioButtons should stand in a column, radioButtonsTwoColumns in two");
    auto wordSize = amplitude();
    wordSize.kind = Kind::Choice;
    wordSize.choices = {"8", "16", "24"};
    require(talas::app::radioButtonColumns(wordSize) == 0, "a symbolList field should be a pop-up, not radio buttons");
    wordSize.fieldKind = Field::RadioButtons;
    require(talas::app::radioButtonColumns(wordSize) == 1, "a radioButtons field should show one column");
    wordSize.fieldKind = Field::RadioButtonsTwoColumns;
    require(talas::app::radioButtonColumns(wordSize) == 2, "a radioButtonsTwoColumns field should show two");

    // The fields of an instance.
    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    auto document = voiceDocument();
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
    auto& voice = document.soundDefinitions.front();
    voice.exposedParameters[0].fieldKind = Field::Value;
    talas::project::ProjectSoundExposedParameter code;
    code.parameterId = "code";
    code.displayName = "Code";
    code.kind = Kind::Text;
    code.fieldKind = Field::CodeString;
    code.defaultValue.parameterId = "code";
    code.defaultValue.text = "3 + 4";
    code.bindings.push_back({talas::project::ProjectSoundParameterTarget::NodeParameter, "osc", "modulation", {}, {}});
    auto label = code;
    label.parameterId = "label";
    label.displayName = "Label";
    label.fieldKind = Field::String;
    label.defaultValue.parameterId = "label";
    label.bindings.front().nodeId = "osc2";
    voice.graph.nodes.push_back({"osc2", "talas.oscillator", {}});
    voice.exposedParameters.push_back(code);
    voice.exposedParameters.push_back(label);
    talas::project::ProjectEditor editor(document, registry);
    const auto added = editor.addSoundInstance("voice1", "voice", {});
    require(added.ok(), "an instance of the voice: " + added.message);
    talas::project::ProjectParameterExpression value;
    value.parameterId = "freq";
    value.expression = "!Pitch";
    const auto event = editor.setParameter("voice1", value);
    require(!event.ok() && event.message.find("value field") != std::string::npos,
        "a value field should refuse an Event Value: " + event.message);
    value.expression = "?pitch";
    require(editor.setParameter("voice1", value).ok(), "a value field should take a variable");
    value.expression = "2 * 220 hz";
    require(editor.setParameter("voice1", value).ok(), "a value field should take an expression");
    // The value type takes any number or expression, but no
    // compound structure such as an Array or a Set.
    for (const auto* compound : {"#(220 440 880)", "Set new", "{220. 440}"})
    {
        value.expression = compound;
        const auto refused = editor.setParameter("voice1", value);
        require(!refused.ok(), std::string{"a value field should refuse "} + compound);
    }
    value.expression = "2 * 220 hz";
    value.expression = "440 hz / 0";
    const auto unevaluated = editor.setParameter("voice1", value);
    require(!unevaluated.ok() && unevaluated.message.find("Division by zero") != std::string::npos,
        "a value field's expression should be evaluated as it is entered: " + unevaluated.message);
    value.parameterId = "amp";
    value.expression = "!Volume";
    require(editor.setParameter("voice1", value).ok(), "a hotValue field should take an Event Value");

    talas::project::ProjectParameterExpression program;
    program.parameterId = "code";
    program.text = "| a |\na := 3 +.\n^ a";
    const auto syntax = editor.setParameter("voice1", program);
    require(!syntax.ok() && syntax.message.find("line 2, column") != std::string::npos
            && syntax.message.find("(at") != std::string::npos,
        "a codeString should be parsed as it is entered, its error told by line and column: " + syntax.message);
    value.parameterId = "freq";
    value.expression = "3 +";
    const auto valueError = editor.setParameter("voice1", value);
    require(!valueError.ok() && valueError.message.find("line") == std::string::npos
            && valueError.message.find("bytes") != std::string::npos,
        "a value field's error gives only the bytes, less than a codeString's: " + valueError.message);
    program.text = "| a |\na := 3 + 4.\n^ a";
    require(editor.setParameter("voice1", program).ok(), "a codeString of sound Smalltalk should be taken");

    talas::project::ProjectParameterExpression typed;
    typed.parameterId = "label";
    typed.text = "?name";
    require(editor.setParameter("voice1", typed).ok(), "a string field should take any text");
    value.expression = "?pitch";
    require(editor.setParameter("voice1", value).ok(), "the value field's variable again");
    const auto variables = talas::project::undeclaredProjectVariables(editor.document());
    require(std::ranges::find(variables, std::string{"name"}) == variables.end()
            && std::ranges::find(variables, std::string{"pitch"}) != variables.end(),
        "a string field cannot be set to a variable; a value field's ?pitch is one");

    // Format 145 saves the field kinds and refuses one of another kind.
    const auto saved = talas::project::serializeProjectDocument(editor.document());
    require(saved.find("\"fieldKind\": \"value\"") != std::string::npos
            && saved.find("\"fieldKind\": \"codeString\"") != std::string::npos,
        "format 145 should save the field kinds");
    const auto loaded = talas::project::parseProjectDocument(saved);
    require(loaded.ok() && loaded.document.soundDefinitions == editor.document().soundDefinitions,
        "the field kinds should load as saved: " + (loaded.errors.empty() ? std::string{} : loaded.errors.front().message));
    auto older = saved;
    older.replace(older.find("\"version\": 156"), 14, "\"version\": 144");
    require(!talas::project::parseProjectDocument(older).ok(), "format 144 should refuse field kinds");
    auto mismatched = saved;
    mismatched.replace(mismatched.find("\"fieldKind\": \"value\""), 20, "\"fieldKind\": \"radioButtons\"");
    require(!talas::project::parseProjectDocument(mismatched).ok(), "an expression's radioButtons field should be refused");
}

// The parameter types, each the kind of field it
// takes, an expression's range in its units or a choice's values; in most
// cases a parameter's type is the one named as it is.
void testTalasParameterTypes()
{
    using Kind = talas::core::ParameterKind;
    using Domain = talas::expr::ValueDomain;
    struct Row
    {
        const char* name;
        Kind kind;
        Domain domain;
        std::optional<double> minimum;
        std::optional<double> maximum;
        std::vector<std::string_view> choices;
    };
    const std::vector<Row> rows{
        {"almostOneScale", Kind::Expression, Domain::Scalar, 0.9, 1.0, {}},
        {"array", Kind::Text, Domain::Scalar, {}, {}, {}},
        {"boolean", Kind::Boolean, Domain::Scalar, {}, {}, {}},
        {"channelNbr", Kind::Choice, Domain::Scalar, {}, {}, {"Mono", "Stereo"}},
        {"codeString", Kind::Text, Domain::Scalar, {}, {}, {}},
        {"complex", Kind::Text, Domain::Scalar, {}, {}, {}},
        {"delayLine", Kind::Resource, Domain::Scalar, {}, {}, {}},
        {"diskFormat", Kind::Choice, Domain::Scalar, {}, {}, {"SDI", "SDII", "AIFF", "IRCAM", "WAVE"}},
        {"doubleScale", Kind::Expression, Domain::Scalar, -2.0, 2.0, {}},
        {"duration", Kind::Expression, Domain::Samples, 1.0, 140737488355327.0, {}},
        {"dynamicsType", Kind::Choice, Domain::Scalar, {}, {}, {"compressor", "expander"}},
        {"envelope", Kind::Text, Domain::Scalar, {}, {}, {}},
        {"envelopeSegmentType", Kind::Choice, Domain::Scalar, {}, {}, {"linear", "exponential"}},
        {"feedbackDelayType", Kind::Choice, Domain::Scalar, {}, {}, {"comb", "allpass"}},
        {"fileName", Kind::Resource, Domain::Scalar, {}, {}, {}},
        {"frequency", Kind::Expression, Domain::Hertz, -25000.0, 25000.0, {}},
        {"genericSource", Kind::Choice, Domain::Scalar, {}, {}, {"Disk", "Live", "RAM"}},
        {"halfScale", Kind::Expression, Domain::Scalar, 0.0, 0.5, {}},
        {"hotArray", Kind::Text, Domain::Scalar, {}, {}, {}},
        {"hotDoubleScale", Kind::Expression, Domain::Scalar, -2.0, 2.0, {}},
        {"hotFrequency", Kind::Expression, Domain::Hertz, -25000.0, 25000.0, {}},
        {"hotPositiveScale", Kind::Expression, Domain::Scalar, 0.0, 1.0, {}},
        {"hotScale", Kind::Expression, Domain::Scalar, -1.0, 1.0, {}},
        {"hotTime", Kind::Expression, Domain::Samples, 0.0, 140737488355327.0, {}},
        {"hotValue", Kind::Expression, Domain::Scalar, {}, {}, {}},
        {"integer", Kind::Expression, Domain::Scalar, -2147483647.0, 2147483647.0, {}},
        {"interpolation", Kind::Choice, Domain::Scalar, {}, {}, {"none", "linear"}},
        {"legalName", Kind::Text, Domain::Scalar, {}, {}, {}},
        {"limeFileName", Kind::Resource, Domain::Scalar, {}, {}, {}},
        {"lowestAnalyzedFreq", Kind::Choice, Domain::Scalar, {}, {},
            {"Above 1F (44 hz)", "Above 2F (87 hz)", "Above 3F (172 hz)", "Above 4F (345 hz)", "Above 5F (690 hz)"}},
        {"midiChannel", Kind::Expression, Domain::Scalar, 1.0, 16.0, {}},
        {"midiControllerNumber", Kind::Expression, Domain::Scalar, 0.0, 127.0, {}},
        {"midiFileName", Kind::Resource, Domain::Scalar, {}, {}, {}},
        {"midiSource", Kind::Choice, Domain::Scalar, {}, {}, {"MIDIInput", "MIDIFile", "Script"}},
        {"modulation", Kind::Choice, Domain::Scalar, {}, {}, {"frequency", "none"}},
        {"name", Kind::Text, Domain::Scalar, {}, {}, {}},
        {"object", Kind::Text, Domain::Scalar, {}, {}, {}},
        {"oscillatorBankInputType", Kind::Choice, Domain::Scalar, {}, {}, {"SpectralShape", "SOSAnalysis"}},
        {"panFunction", Kind::Choice, Domain::Scalar, {}, {}, {"linear", "power"}},
        {"polesAndZeroes", Kind::Text, Domain::Scalar, {}, {}, {}},
        {"positiveFrequency", Kind::Expression, Domain::Hertz, 0.001, 25000.0, {}},
        {"positiveInteger", Kind::Expression, Domain::Scalar, 0.0, 214748364.0, {}},
        {"positiveScale", Kind::Expression, Domain::Scalar, 0.0, 1.0, {}},
        {"processor", Kind::Expression, Domain::Scalar, 1.0, 8.0, {}},
        {"samplesFileList", Kind::Text, Domain::Scalar, {}, {}, {}},
        {"samplesFileName", Kind::Resource, Domain::Scalar, {}, {}, {}},
        {"scale", Kind::Expression, Domain::Scalar, -1.0, 1.0, {}},
        {"shapeSource", Kind::Choice, Domain::Scalar, {}, {}, {"wavetable", "polynomial"}},
        {"singleWavetableDelay", Kind::Expression, Domain::Samples, 0.0, 4096.0, {}},
        {"spectrumFileName", Kind::Resource, Domain::Scalar, {}, {}, {}},
        {"spectrumWavetable", Kind::Resource, Domain::Scalar, {}, {}, {}},
        {"string", Kind::Text, Domain::Scalar, {}, {}, {}},
        {"textFileName", Kind::Resource, Domain::Scalar, {}, {}, {}},
        {"time", Kind::Expression, Domain::Samples, 0.0, 140737488355327.0, {}},
        {"upScale", Kind::Expression, Domain::Scalar, -16.0, 16.0, {}},
        {"value", Kind::Expression, Domain::Scalar, {}, {}, {}},
        {"wavetable", Kind::Resource, Domain::Scalar, {}, {}, {}},
        {"wavetableAccess", Kind::Choice, Domain::Scalar, {}, {}, {"none", "readWrite", "readOnly", "writeOnly"}},
        {"wavetableDelay", Kind::Expression, Domain::Samples, 0.0, 4096.0, {}},
        {"wavetableOfStandardLength", Kind::Resource, Domain::Scalar, {}, {}, {}},
        {"wordSize", Kind::Choice, Domain::Scalar, {}, {}, {"8", "16", "24"}},
    };
    require(talas::app::talasParameterTypes().size() == rows.size(), "every documented type with a field kind should be listed");
    require(std::ranges::is_sorted(talas::app::talasParameterTypes(), {}, &talas::app::TalasParameterType::name),
        "the types should be listed alphabetically");
    for (const auto& row : rows)
    {
        const auto* type = talas::app::findTalasParameterType(row.name);
        require(type != nullptr && type->kind == row.kind && type->domain == row.domain && type->minimum == row.minimum
                && type->maximum == row.maximum && std::ranges::equal(type->choices, row.choices)
                && type->hot == std::string_view{row.name}.starts_with("hot"),
            std::string{"the "} + row.name + " type should take its listed kind, range and choices");
    }
    // A type applies to a parameter of its field's kind: an expression its
    // domain and range, which then bound the default.
    std::string error;
    const auto hertz = talas::app::applyTalasParameterType(frequency(), *talas::app::findTalasParameterType("frequency"), error);
    require(hertz && hertz->domain == talas::expr::ValueDomain::Hertz && hertz->minimum == -25000.0
            && hertz->maximum == 25000.0,
        "a frequency parameter should range from -25000 to 25000 hz: " + error);
    const auto half = talas::app::applyTalasParameterType(amplitude(), *talas::app::findTalasParameterType("halfScale"), error);
    require(half && half->minimum == 0.0 && half->maximum == 0.5, "halfScale should range from 0 to 0.5: " + error);
    require(!talas::app::applyTalasParameterType(amplitude(), *talas::app::findTalasParameterType("boolean"), error),
        "a type of another kind should be refused");
    auto stereo = amplitude();
    stereo.parameterId = "channelNbr";
    stereo.kind = talas::core::ParameterKind::Choice;
    stereo.choices = {"Mono", "Stereo"};
    require(talas::app::talasParameterTypeOf(stereo) == talas::app::findTalasParameterType("channelNbr")
            && talas::app::talasParameterTypeOf(frequency()) == nullptr,
        "a parameter's type should be the one named as it is, where its field takes it");
    stereo.choices = {"Left", "Right"};
    require(talas::app::talasParameterTypeOf(stereo) == nullptr
            && !talas::app::talasParameterTypeFits(stereo, *talas::app::findTalasParameterType("channelNbr")),
        "a choice type should fit only a parameter with its values");
}

// A parameter's type defines its range of legal
// values, its default value and, for frequency and duration fields, its
// default units. A type keeps a default it takes and otherwise gives its
// own, the legal value nearest 0 in its units.
void testATypeGivesItsDefaultValueAndUnits()
{
    using Domain = talas::expr::ValueDomain;
    const auto valueIn = [](const std::string& text, Domain domain) {
        const auto parsed = talas::expr::parse(text, talas::expr::ExpressionLanguageVersion::ValV1);
        require(parsed.ok(), "the default " + text + " should parse");
        const auto evaluated = talas::expr::evaluate(*parsed.expression, {}, {domain});
        require(evaluated.ok(), "the default " + text + " should evaluate");
        return evaluated.value.value;
    };
    std::string error;
    auto silent = frequency();
    silent.defaultValue.expression = "0";
    const auto positive = talas::app::applyTalasParameterType(silent, *talas::app::findTalasParameterType("positiveFrequency"), error);
    require(positive && positive->minimum == 0.001 && positive->defaultValue.expression == std::optional<std::string>{"0.001 hz"}
            && positive->defaultValue.parameterId == "freq",
        "positiveFrequency should give a default of its own, its lowest legal value in hz: " + error);
    const auto kept = talas::app::applyTalasParameterType(frequency(), *talas::app::findTalasParameterType("positiveFrequency"), error);
    require(kept && kept->defaultValue.expression == std::optional<std::string>{"440 hz"},
        "a default the type takes should be kept: " + error);
    // Default units: a bare number in a frequency-typed parameter is hertz.
    const auto hertz = talas::app::applyTalasParameterType(amplitude(), *talas::app::findTalasParameterType("frequency"), error);
    require(hertz && hertz->domain == Domain::Hertz && hertz->defaultValue.expression == std::optional<std::string>{"0.5"}
            && valueIn("0.5", hertz->domain) == valueIn("0.5 hz", hertz->domain),
        "a frequency type's default units should be hertz: " + error);
    // A duration type's range is in samples, so its units are too.
    auto timed = amplitude();
    timed.defaultValue.expression = "0";
    const auto duration = talas::app::applyTalasParameterType(timed, *talas::app::findTalasParameterType("duration"), error);
    require(duration && duration->domain == Domain::Samples
            && duration->defaultValue.expression == std::optional<std::string>{"1 samp"}
            && valueIn("1", duration->domain) == valueIn("1 samp", duration->domain),
        "a duration type should give a default of 1 samp and read a bare number in samples: " + error);
    const auto channel = talas::app::applyTalasParameterType(timed, *talas::app::findTalasParameterType("midiChannel"), error);
    require(channel && channel->defaultValue.expression == std::optional<std::string>{"1"},
        "a MIDI channel's default should be its lowest channel, 1: " + error);
    auto live = silent;
    live.defaultValue.expression = "!Frequency";
    const auto played = talas::app::applyTalasParameterType(live, *talas::app::findTalasParameterType("positiveFrequency"), error);
    require(played && played->defaultValue.expression == std::optional<std::string>{"!Frequency"},
        "an Event Value default plays as it comes and should be kept: " + error);
    auto stereo = amplitude();
    stereo.kind = talas::core::ParameterKind::Choice;
    stereo.choices = {"Mono", "Stereo"};
    stereo.defaultValue = {"amp", {}, std::string{"Quad"}, {}, {}, {}};
    const auto channels = talas::app::applyTalasParameterType(stereo, *talas::app::findTalasParameterType("channelNbr"), error);
    require(channels && channels->defaultValue.choice == std::optional<std::string>{"Mono"},
        "a choice type should give its first value where the default is none of its values: " + error);
    stereo.defaultValue.choice = "Stereo";
    require(talas::app::applyTalasParameterType(stereo, *talas::app::findTalasParameterType("channelNbr"), error)->defaultValue.choice
            == std::optional<std::string>{"Stereo"},
        "a choice the type takes should be kept");
    auto flag = amplitude();
    flag.kind = talas::core::ParameterKind::Boolean;
    flag.defaultValue = {"amp", {}, {}, {}, {}, {}};
    const auto checked = talas::app::applyTalasParameterType(flag, *talas::app::findTalasParameterType("boolean"), error);
    require(checked && checked->defaultValue.boolean == std::optional<bool>{false},
        "the boolean type should give false as its default: " + error);
    require(!talas::app::talasParameterTypeDefault(*talas::app::findTalasParameterType("complex")).has_value()
            && !talas::app::talasParameterTypeDefault(*talas::app::findTalasParameterType("fileName")).has_value(),
        "text and file types should give no default of their own");
}

// Edit Class
// edits the class of the Sound selected, or of the Sound being edited.
void testTheClassEditClassEdits()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition voice;
    voice.definitionId = "voice";
    voice.graph.nodes.push_back({"osc", "talas.oscillator", {}});
    document.soundDefinitions.push_back(voice);
    talas::project::ProjectNode instance;
    instance.instanceId = "voice1";
    instance.soundDefinitionId = "voice";
    document.graph.nodes = {instance, {"noise", "talas.noise", {}}};
    require(talas::app::classOfSelection(document, std::nullopt, "voice1") == std::optional<std::string>{"voice"},
        "Edit class edits the selected Sound's class");
    require(talas::app::classOfSelection(document, std::string{"voice"}, "osc") == std::optional<std::string>{"voice"},
        "inside a class's signal flow, its own class");
    require(!talas::app::classOfSelection(document, std::nullopt, "noise").has_value()
            && !talas::app::classOfSelection(document, std::string{"missing"}, "osc").has_value(),
        "a plain Sound of the project has no class of its own to edit");
}

// Another Sound dragged onto
// the Parameter list gives each parameter the type of its namesake there.
void testParameterTypesSameAsAnotherSound()
{
    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    const auto exposed = [](std::string id, talas::expr::ValueDomain domain) {
        talas::project::ProjectSoundExposedParameter parameter;
        parameter.parameterId = id;
        parameter.displayName = id;
        parameter.domain = domain;
        parameter.defaultValue.parameterId = std::move(id);
        parameter.defaultValue.expression = "1";
        return parameter;
    };
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition voice;
    voice.definitionId = "voice";
    auto timed = exposed("level", talas::expr::ValueDomain::Seconds);
    timed.minimum = 0.0;
    timed.maximum = 2.0;
    timed.comment = "How long the voice holds";
    timed.field = talas::project::ProjectSoundFieldRectangle{0.5, 0.0, 1.0, 0.25};
    timed.fieldKind = talas::project::ProjectSoundFieldKind::HotValue;
    voice.exposedParameters = {timed};
    document.soundDefinitions.push_back(voice);
    talas::project::ProjectSoundDefinition pad;
    pad.definitionId = "pad";
    pad.exposedParameters = {exposed("frequency", talas::expr::ValueDomain::Scalar), exposed("level", talas::expr::ValueDomain::Scalar)};
    const auto fromOscillator = talas::app::classParametersTypedAs(pad, document, registry, "talas.oscillator");
    require(fromOscillator == std::optional<std::vector<std::string>>{std::vector<std::string>{"frequency"}}
            && pad.exposedParameters[0].domain == talas::expr::ValueDomain::Hertz && pad.exposedParameters[0].maximum == 25000.0
            && pad.exposedParameters[1].domain == talas::expr::ValueDomain::Scalar,
        "an Oscillator dragged onto the parameters should type Frequency as its Frequency, leaving the rest");
    const auto fromVoice = talas::app::classParametersTypedAs(pad, document, registry, "voice");
    require(fromVoice == std::optional<std::vector<std::string>>{std::vector<std::string>{"level"}}
            && pad.exposedParameters[1].domain == talas::expr::ValueDomain::Seconds && pad.exposedParameters[1].maximum == 2.0,
        "a Sound class dragged there should give its own parameters' types");
    require(pad.exposedParameters[1].comment == timed.comment && pad.exposedParameters[1].field == timed.field
            && pad.exposedParameters[1].fieldKind == talas::project::ProjectSoundFieldKind::HotValue
            && pad.exposedParameters[0].comment.empty(),
        "an instance of the old class should give back its comments and field places too");
    require(!talas::app::classParametersTypedAs(pad, document, registry, "missing").has_value(),
        "a drag naming no Sound should change nothing");
}

// Retrieve example
// from class gives back the Sound the class was made from, its variables
// as they were, as a plain Sound among the others.
void testRetrievingTheExample()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition pad;
    pad.definitionId = "pad";
    pad.displayName = "Pad";
    pad.graph.nodes.push_back({"osc", "talas.oscillator", {{"frequency", "?freq"}}});
    pad.outputs.push_back({"audioOut", talas::core::PortType::Audio, "osc", "audioOut"});
    pad.exposedParameters.push_back(frequency());
    pad.exposedParameters.back().bindings = {{talas::project::ProjectSoundParameterTarget::Variable, {}, {}, {}, {}, "freq"}};
    pad.description = "A pad.";
    document.soundDefinitions.push_back(pad);
    const auto example = talas::app::classExample(document, "pad");
    require(example.has_value() && example->definitionId == "padExample" && example->displayName == "Pad example"
            && example->graph == pad.graph && example->outputs == pad.outputs && example->exposedParameters.empty()
            && example->description.empty(),
        "Retrieve example from class should give the Sound it was made from, ?freq and all, as a plain Sound");
    document.soundDefinitions.push_back(*example);
    require(talas::app::classExample(document, "pad")->displayName == "Pad example1"
            && talas::app::classExample(document, "pad")->definitionId == "padExample1",
        "a second example should be numbered");
    require(!talas::app::classExample(document, "missing").has_value(), "no class, no example");
}

// Retrieve example from class works on the classes
// the user makes and on a few, though not all, of the System classes. Here
// the System prototypes built from other Sounds give the Sounds they are
// built from (AR Drag&Drop: a VCA whose Gain an AR keyed by !KeyDown and
// !KeyVelocity sets, over a Noise), and a System class
// of one Sound, built in, has no example and says so.
void testRetrievingASystemClassesExample()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument document;
    std::string error;
    const auto example = talas::app::systemClassExample(document, registry, talas::app::arDragAndDropPrototypeId, error);
    require(example.has_value() && error.empty(), "AR Drag&Drop, built from other Sounds, should give its example: " + error);
    require(example->definitionId == "arDragAndDropExample" && example->displayName == "AR Drag&Drop example"
            && example->exposedParameters.empty() && !example->icon.has_value(),
        "the example should be a plain Sound named after the System class");
    const auto node = [&](std::string_view typeId) {
        return std::ranges::find(example->graph.nodes, typeId, &talas::project::ProjectNode::typeId);
    };
    const auto vca = node("talas.vca");
    const auto envelope = node("talas.ar");
    const auto noise = node("talas.noise");
    require(example->graph.nodes.size() == 3 && vca != example->graph.nodes.end() && envelope != example->graph.nodes.end()
            && noise != example->graph.nodes.end(),
        "AR Drag&Drop's example should hold its VCA, its AR and its Noise");
    const auto field = [](const talas::project::ProjectNode& sound, std::string_view id) {
        const auto parameter = std::ranges::find(sound.parameters, id, &talas::project::ProjectParameterExpression::parameterId);
        return parameter == sound.parameters.end() ? std::optional<std::string>{} : parameter->expression;
    };
    const auto joins = [&](const talas::core::Connection& connection) {
        return std::ranges::find(example->graph.connections, connection) != example->graph.connections.end();
    };
    require(joins({envelope->instanceId, "value", vca->instanceId, "gain"})
            && joins({noise->instanceId, "audioOut", vca->instanceId, "audioIn"})
            && field(*envelope, "gate") == std::optional<std::string>{"!KeyDown"}
            && field(*envelope, "scale") == std::optional<std::string>{"!KeyVelocity"}
            && example->outputs.size() == 1 && example->outputs.front().nodeId == vca->instanceId,
        "the example should be the Sounds the class is built from: the AR keyed by !KeyDown setting the VCA's Gain "
        "over the Noise");
    talas::project::ProjectEditor editor(document, registry);
    require(editor.addSoundDefinition(*example).ok() && talas::project::validateProjectDocument(editor.document(), registry).ok(),
        "the retrieved example should be a Sound of the project like any other");
    const auto again = talas::app::systemClassExample(editor.document(), registry, talas::app::arDragAndDropPrototypeId, error);
    require(again.has_value() && again->displayName == "AR Drag&Drop example1" && again->definitionId == "arDragAndDropExample1",
        "a second example should be numbered");

    require(!talas::app::systemClassExample(document, registry, "talas.oscillator", error).has_value()
            && error.find("Oscillator") != std::string::npos && error.find("no example") != std::string::npos,
        "a System class of one Sound, built in, should have no example, and say so: " + error);
    require(!talas::app::systemClassExample(document, registry, "talas.noSuchPrototype", error).has_value() && !error.empty(),
        "an id naming no System prototype should give no example");

    // A few, though not all: among the System prototypes, those built from
    // other Sounds give an example, the rest none.
    std::size_t withExample = 0;
    const auto palette = talas::app::prototypePalette(registry);
    for (const auto& prototype : palette)
    {
        const auto retrieved = talas::app::systemClassExample(document, registry, prototype.typeId, error);
        require(retrieved.has_value() == talas::app::isPrototypeAssembly(prototype.typeId) && retrieved.has_value() == error.empty(),
            "" + prototype.displayName + " should give an example only when built from other Sounds: " + error);
        withExample += retrieved.has_value() ? 1 : 0;
    }
    require(withExample >= 1 && withExample < palette.size(), "a few System classes, though not all, should have an example");

    // Selected in the signal flow, a Sound of no class is its System
    // class's instance; a class's instance is the class's.
    talas::project::ProjectDocument flow;
    flow.soundDefinitions.push_back(*example);
    flow.graph.nodes = {{"osc", "talas.oscillator", {}}, {"pad", {}, {}, std::string{"arDragAndDropExample"}, {}}};
    require(talas::app::systemClassOfSelection(flow, std::nullopt, "osc") == std::optional<std::string>{"talas.oscillator"}
            && !talas::app::systemClassOfSelection(flow, std::nullopt, "pad").has_value()
            && talas::app::systemClassOfSelection(flow, std::string{"arDragAndDropExample"}, vca->instanceId)
                == std::optional<std::string>{"talas.vca"}
            && !talas::app::systemClassOfSelection(flow, std::nullopt, "missing").has_value(),
        "the System class of the Sound selected should be its prototype, and a class's instance none");
}

// Closing the editor the first time
// on a new class asks whether to save an instance of it, made with the
// parameters' default values; No starts over, in one edit Undo takes back.
void testClosingANewClassAsksToSaveAnInstance()
{
    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"osc", "talas.oscillator", {{.parameterId = "frequency", .expression = std::string{"220 hz"}}}, {}, {}});
    talas::project::ProjectEditor editor(document, registry);
    const auto before = talas::project::serializeProjectDocument(editor.document());
    const auto start = editor.document();
    require(editor.extractNodesToSound({"osc"}, "tone", "Tone", "tone1").ok(), "the example should become a class");
    const auto instances = talas::app::newClassInstances(editor.document(), "tone");
    const auto instance = std::ranges::find(editor.document().graph.nodes, std::string{"tone1"}, &talas::project::ProjectNode::instanceId);
    require(instances == std::vector<std::string>{"tone1"} && instance != editor.document().graph.nodes.end()
            && instance->parameters.empty(),
        "the new class's instance should take its parameters' default values");
    const auto question = talas::app::newClassCloseQuestion("Tone");
    require(question.find("Save an instance") != std::string::npos && question.find("Tone") != std::string::npos
            && question.find("starts over") != std::string::npos,
        "the first close should ask whether to save an instance, No starting over");
    require(editor.applyDocumentEdit(start).ok() && talas::project::serializeProjectDocument(editor.document()) == before
            && talas::app::newClassInstances(editor.document(), "tone").empty(),
        "No should start over from scratch, without the class");
    require(editor.undo().ok() && talas::app::newClassInstances(editor.document(), "tone").size() == 1,
        "starting over should be one edit Undo takes back");
}

// New Class from
// Example on a Sound of the Sound file window makes the class from it, the
// variables in its fields the class's parameters, the example left as it
// is; Yes on the editor's first close saves an instance of the class in the
// Sound file window, named Stereo as the class is, highlighted (selected
// alone), its parameters at their defaults.
void testANewClassFromTheSoundFileWindowSavesAnInstanceThere()
{
    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    talas::project::ProjectDocument document;
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
    talas::project::ProjectSoundDefinition example;
    example.definitionId = "locator";
    example.displayName = "Locator";
    example.graph.nodes.push_back({"osc", "talas.oscillator", {{"frequency", "?freq"}}});
    example.outputs.push_back({"audioOut", talas::core::PortType::Audio, "osc", "audioOut"});
    document.soundDefinitions.push_back(example);
    talas::project::ProjectEditor editor(document, registry);
    const auto made = talas::project::classFromSoundFileExample(editor.document(), registry, "locator", "stereo", "Stereo");
    require(made.has_value() && made->displayName == "Stereo" && made->graph == example.graph
            && made->exposedParameters.size() == 1 && made->exposedParameters.front().parameterId == "freq"
            && made->exposedParameters.front().bindings.front().target == talas::project::ProjectSoundParameterTarget::Variable,
        "the class should be made from the Sound file window's Sound, its ?freq a parameter");
    require(!talas::project::classFromSoundFileExample(editor.document(), registry, "missing", "x", "X").has_value()
            && !talas::project::classFromSoundFileExample(editor.document(), registry, "locator", "locator", "Again").has_value(),
        "no class from a Sound the file does not have, nor under an id taken");
    require(editor.addSoundDefinition(*made).ok() && editor.document().soundDefinitions.front() == example,
        "the class should be added, the example left as it was");
    const auto saved = talas::app::classInstanceSavedInSoundFile(editor.document(), "stereo");
    require(saved.has_value() && saved->selection == std::vector<std::string>{"stereoInstance"},
        "the instance saved should be the Sound file window's selection, highlighted");
    const auto icons = talas::app::soundFileIcons(saved->document);
    const auto icon = std::ranges::find(icons, std::string{"stereoInstance"}, &talas::app::SoundFileIcon::definitionId);
    require(icon != icons.end() && icon->name == "Stereo",
        "the new class's instance should appear in the Sound file window, named Stereo");
    const auto& instance = saved->document.soundDefinitions.back();
    require(instance.graph.nodes.size() == 1 && instance.graph.nodes.front().soundDefinitionId == std::optional<std::string>{"stereo"}
            && instance.graph.nodes.front().parameters.empty() && instance.outputs.size() == 1
            && instance.outputs.front().nodeId == instance.graph.nodes.front().instanceId,
        "the instance should be one Sound of the class, its parameters at their defaults, playing out");
    require(editor.applyDocumentEdit(saved->document).ok(),
        "the project with the instance saved should be a valid one");
    require(talas::app::classInstanceSavedInSoundFile(editor.document(), "stereo")->selection
                == std::vector<std::string>{"stereoInstance2"},
        "a second instance should take the next id free");
    require(!talas::app::classInstanceSavedInSoundFile(editor.document(), "missing").has_value(), "no class, no instance");
}

// New Class from Example asks for
// the free variables' values first; they become the parameters' defaults.
void testTheExamplesValuesBecomeTheDefaults()
{
    talas::project::ProjectSoundDefinition locator;
    locator.definitionId = "locator";
    talas::project::ProjectSoundExposedParameter left;
    left.parameterId = "left";
    left.defaultValue.parameterId = "left";
    left.defaultValue.expression = "1.0";
    left.bindings.push_back({talas::project::ProjectSoundParameterTarget::Variable, {}, {}, {}, talas::expr::ValueDomain::Scalar, "left"});
    talas::project::ProjectSoundExposedParameter fixed;
    fixed.parameterId = "scale";
    fixed.defaultValue.parameterId = "scale";
    fixed.defaultValue.expression = "0.5";
    talas::project::ProjectSoundExposedParameter free;
    free.parameterId = "spread";
    free.defaultValue.parameterId = "spread";
    free.defaultValue.expression = "?spread";
    free.bindings.push_back({talas::project::ProjectSoundParameterTarget::Variable, {}, {}, {}, talas::expr::ValueDomain::Scalar, "spread"});
    locator.exposedParameters = {left, fixed, free};
    const auto made = talas::app::classWithDefaults(locator, {{"left", 0.75}});
    require(made.exposedParameters[0].defaultValue.expression == std::optional<std::string>{"0.75"},
        "the value given ?left should be the Left parameter's default");
    require(made.exposedParameters[1].defaultValue.expression == std::optional<std::string>{"0.5"}
            && made.exposedParameters[2].defaultValue.expression == std::optional<std::string>{"?spread"},
        "a parameter not a variable, or a variable given no value, should keep its default");
}

// The codeString field is the Script field
// of a Script and of the FileInterpreters, its Smalltalk parsed as it is
// entered and a syntax error told by line and column, and the
// ParameterTransformer's Transformation field, parsed as a transformation.
void testScriptFieldsAreCodeStrings()
{
    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    talas::project::ProjectDocument document;
    for (const auto& [id, type] : {std::pair{"score", "talas.script"}, std::pair{"text", "talas.textFileInterpreter"},
             std::pair{"midi", "talas.midiFileInterpreter"}, std::pair{"lime", "talas.limeInterpreter"},
             std::pair{"transformer", "talas.parameterTransformer"}})
    {
        document.graph.nodes.push_back({id, type, {}});
    }
    talas::project::ProjectEditor editor(document, registry);
    talas::project::ProjectParameterExpression script;
    script.parameterId = "script";
    for (const auto* nodeId : {"score", "text", "midi", "lime"})
    {
        script.text = "| a |\na := 3 +.\n^ a";
        const auto refused = editor.setParameter(nodeId, script);
        require(!refused.ok() && refused.message.find("Script, line 2, column") != std::string::npos,
            std::string{"the Script field of "} + nodeId + " should be a codeString, its error told by line and column: "
                + refused.message);
        script.text = "| a |\na := 3 + 4.";
        require(editor.setParameter(nodeId, script).ok(), std::string{"a sound script should be taken by "} + nodeId);
    }
    talas::project::ProjectParameterExpression transformation;
    transformation.parameterId = "transformation";
    transformation.text = "snd frequency: (";
    require(!editor.setParameter("transformer", transformation).ok(),
        "the Transformation field should refuse code that does not parse");
}

// Format 147: a complex field takes a number or a complex number, real %
// imaginary; a polesAndZeroes field a list of them; a grammar field a
// ContextFreeGrammar's production rules; and the object type, the least
// restrictive, any Smalltalk object at all, as Smalltalk code.
void testComplexPolesGrammarAndObjectFields()
{
    using Kind = talas::core::ParameterKind;
    using Field = talas::project::ProjectSoundFieldKind;
    require(std::ranges::equal(talas::app::classEditorFieldKinds(Kind::Text),
                std::vector<std::string_view>{"string", "codeString", "complex", "polesAndZeroes", "grammar"}),
        "a text should offer the complex, polesAndZeroes and grammar fields");
    std::string error;
    auto text = amplitude();
    text.kind = Kind::Text;
    text.defaultValue.expression.reset();
    text.defaultValue.text = "0";
    const auto pole = talas::app::applyTalasParameterType(text, *talas::app::findTalasParameterType("complex"), error);
    const auto poles = talas::app::applyTalasParameterType(text, *talas::app::findTalasParameterType("polesAndZeroes"), error);
    const auto anything = talas::app::applyTalasParameterType(text, *talas::app::findTalasParameterType("object"), error);
    require(pole && pole->fieldKind == Field::Complex && poles && poles->fieldKind == Field::PolesAndZeroes && anything
            && anything->fieldKind == Field::CodeString,
        "complex and polesAndZeroes should take their own fields, object a codeString: " + error);
    require(!talas::app::applyTalasParameterType(amplitude(), *talas::app::findTalasParameterType("object"), error),
        "an object is Smalltalk code, not an expression field");

    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    auto document = voiceDocument();
    auto& voice = document.soundDefinitions.front();
    const auto textParameter = [&](std::string id, Field field, std::string value) {
        talas::project::ProjectSoundExposedParameter parameter;
        parameter.parameterId = id;
        parameter.displayName = id;
        parameter.kind = Kind::Text;
        parameter.fieldKind = field;
        parameter.defaultValue.parameterId = id;
        parameter.defaultValue.text = std::move(value);
        voice.graph.nodes.push_back({id + "Node", "talas.oscillator", {}});
        parameter.bindings.push_back({talas::project::ProjectSoundParameterTarget::NodeParameter, id + "Node", "modulation", {}, {}});
        voice.exposedParameters.push_back(std::move(parameter));
    };
    textParameter("pole", Field::Complex, "0.5 % 0.25");
    textParameter("poles", Field::PolesAndZeroes, "{0.9 % 0.1. 0.9 % -0.1}");
    textParameter("rules", Field::Grammar, "A -> (s1) [1] | (s2) [1].");
    textParameter("anything", Field::CodeString, "nil");
    talas::project::ProjectEditor editor(document, registry);
    require(editor.addSoundInstance("voice1", "voice", {}).ok(), "an instance of the voice");
    const auto set = [&](std::string id, std::string value) {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = std::move(id);
        parameter.text = std::move(value);
        return editor.setParameter("voice1", parameter);
    };
    for (const auto* accepted : {"0.5 % 0.25", "-1 % 2", "0.75"})
        require(set("pole", accepted).ok(), std::string{"a complex field should take "} + accepted);
    for (const auto* refused : {"#(1 2)", "'pole'", "0.5 %"})
    {
        const auto result = set("pole", refused);
        require(!result.ok(), std::string{"a complex field should refuse "} + refused);
    }
    for (const auto* accepted : {"{0.9 % 0.1. 0.9 % -0.1. 0.5}", "#(0.5 0.25)"})
        require(set("poles", accepted).ok(), std::string{"a polesAndZeroes field should take "} + accepted);
    for (const auto* refused : {"0.9 % 0.1", "{'a'}", "{0.9 %}"})
        require(!set("poles", refused).ok(), std::string{"a polesAndZeroes field should refuse "} + refused);
    require(set("rules", "A -> ((s1 , A) , (s1)) [2] | (s2) [1].").ok(),
        "a grammar field should take a ContextFreeGrammar's production rules");
    const auto badRules = set("rules", "A -> (s1)");
    require(!badRules.ok() && badRules.message.find("grammar field") != std::string::npos,
        "a grammar field should refuse rules that do not read: " + badRules.message);
    for (const auto* object : {"#(1 2 3)", "'a string'", "#symbol", "3 % 4", "Set new", "[:x | x * 2]"})
        require(set("anything", object).ok(), std::string{"an object should take any Smalltalk object: "} + object);
    require(!set("anything", "3 +").ok(), "an object's Smalltalk should still read");

    // Format 147 saves the new fields; format 146 refuses them.
    const auto saved = talas::project::serializeProjectDocument(editor.document());
    require(saved.find("\"fieldKind\": \"complex\"") != std::string::npos
            && saved.find("\"fieldKind\": \"polesAndZeroes\"") != std::string::npos
            && saved.find("\"fieldKind\": \"grammar\"") != std::string::npos,
        "format 147 should save the complex, polesAndZeroes and grammar fields");
    const auto loaded = talas::project::parseProjectDocument(saved);
    require(loaded.ok() && loaded.document.soundDefinitions == editor.document().soundDefinitions,
        "the new fields should load as saved: " + (loaded.errors.empty() ? std::string{} : loaded.errors.front().message));
    auto older = saved;
    older.replace(older.find("\"version\": 156"), 14, "\"version\": 146");
    require(!talas::project::parseProjectDocument(older).ok(), "format 146 should refuse the new fields");
}

int main()
{
    try
    {
        testTheClassEditClassEdits();
        testParameterTypesSameAsAnotherSound();
        testRetrievingTheExample();
        testRetrievingASystemClassesExample();
        testTalasParameterTypes();
        testATypeGivesItsDefaultValueAndUnits();
        testFieldRectangles();
        testParameterFields();
        testProjectClassFields();
        testParameterFieldKinds();
        testScriptFieldsAreCodeStrings();
        testClosingAsksAboutChangesNotKept();
        testClosingANewClassAsksToSaveAnInstance();
        testANewClassFromTheSoundFileWindowSavesAnInstanceThere();
        testTheExamplesValuesBecomeTheDefaults();
        testComplexPolesGrammarAndObjectFields();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "class editor model tests passed\n";
    return 0;
}
