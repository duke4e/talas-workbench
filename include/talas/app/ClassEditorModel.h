#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The Class Editor of a Sound definition's parameters: their types, ranges, field
// rectangles and comments, and the class description.
namespace talas::app
{
// The Field Rectangle written as text, `0 @ 0 corner: 1 @ 0.25`, and back
// (spaces around @ optional); nothing for other text or a rectangle outside 0
// to 1 or with its corners out of order.
[[nodiscard]] std::string fieldRectangleText(const talas::project::ProjectSoundFieldRectangle& rectangle);
[[nodiscard]] std::optional<talas::project::ProjectSoundFieldRectangle> parseFieldRectangle(std::string_view text);

// The Field Layout, a miniature Sound editor: where each parameter's field
// lies, by its Field Rectangle, the selected parameter's highlighted gray; a
// parameter with no rectangle has no place drawn. A field overlapping another
// is marked, for its rectangle to be moved; fields may touch.
struct ClassEditorLayoutBox
{
    std::string name;
    talas::project::ProjectSoundFieldRectangle field;
    bool highlighted = false;
    bool overlaps = false;

    bool operator==(const ClassEditorLayoutBox&) const = default;
};

[[nodiscard]] std::vector<ClassEditorLayoutBox> classEditorFieldLayout(
    const talas::project::ProjectSoundDefinition& definition, std::string_view selectedParameterId);

// What an instance's parameter shows beside its field: its comment, then
// its range, as `Range 0 to 10`; with neither, that no help is available
//.
[[nodiscard]] std::string soundParameterHelp(const talas::project::ProjectSoundExposedParameter& parameter);

// A parameter edited in the Class Editor: its name, the domain of an
// expression (scalar, hertz, seconds, note number, samples), the range
// (empty text for none), the field rectangle (empty for none), the
// comment and the kind of field (empty to keep it); nothing, with `error`
// set, for fields that do not read.
struct ClassEditorParameterFields
{
    std::string displayName;
    std::string domain;
    std::string minimum;
    std::string maximum;
    std::string field;
    std::string comment;
    std::string fieldKind;
};

// The Class Editor's Field list: the kinds of field a parameter of a kind can have, the
// default first: hotValue or value for an expression, string, codeString,
// complex, polesAndZeroes or grammar for a text (the last three from format
// 147), symbolList or radio buttons in one or two columns
// for a choice, boolean for a check box and fileName for a file.
[[nodiscard]] std::vector<std::string_view> classEditorFieldKinds(talas::core::ParameterKind kind);
// The kind of field a parameter has, by its name in the Field list.
[[nodiscard]] std::string_view classEditorFieldKindName(const talas::project::ProjectSoundExposedParameter& parameter);

// A radioButtons field shows a choice's values as a column of radio
// buttons, radioButtonsTwoColumns in two columns, the first filled first
//: the column and row of each of `count` buttons.
struct RadioButtonPlace
{
    std::size_t column = 0;
    std::size_t row = 0;

    bool operator==(const RadioButtonPlace&) const = default;
};
[[nodiscard]] std::vector<RadioButtonPlace> radioButtonPlaces(std::size_t count, std::size_t columns);
// The columns of radio buttons a parameter's field shows; 0 for a field of
// another kind.
[[nodiscard]] std::size_t radioButtonColumns(const talas::project::ProjectSoundExposedParameter& parameter) noexcept;

[[nodiscard]] ClassEditorParameterFields classEditorFields(const talas::project::ProjectSoundExposedParameter& parameter);
[[nodiscard]] std::optional<talas::project::ProjectSoundExposedParameter> applyClassEditorFields(
    talas::project::ProjectSoundExposedParameter parameter, const ClassEditorParameterFields& fields, std::string& error);

// Closing the Class Editor: it
// asks whether to save the changes not yet kept, a parameter's fields that
// would change it, or that do not read, among them.
[[nodiscard]] bool classEditorFieldsChanged(
    const talas::project::ProjectSoundExposedParameter& parameter, const ClassEditorParameterFields& fields);

[[nodiscard]] std::string_view classEditorDomainName(talas::expr::ValueDomain domain);

// The parameter types: the kind of field each
// takes, an expression's domain and range in that domain's units, a
// choice's values, and whether it is one of the hot types. These do not tell
// hot from cold by themselves; the field does, a value field taking no
// Event Values, and a hot type selects a hotValue field. The complex and
// polesAndZeroes types take fields of their own and object a codeString
// (format 147); the sound, soundCollection and legalNameSoundCollection
// types have no field kind here.
struct TalasParameterType
{
    std::string_view name;
    talas::core::ParameterKind kind = talas::core::ParameterKind::Expression;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    std::optional<double> minimum;
    std::optional<double> maximum;
    std::span<const std::string_view> choices;
    bool hot = false;
    // A description of what it takes.
    std::string_view takes;
};

[[nodiscard]] std::span<const TalasParameterType> talasParameterTypes() noexcept;
[[nodiscard]] const TalasParameterType* findTalasParameterType(std::string_view name) noexcept;

// In most cases a parameter's type is the one named as it is.
[[nodiscard]] const TalasParameterType* talasParameterTypeOf(const talas::project::ProjectSoundExposedParameter& parameter) noexcept;

// The types a parameter can take: those of its field's kind, and for a
// choice those whose values are its choices.
[[nodiscard]] bool talasParameterTypeFits(
    const talas::project::ProjectSoundExposedParameter& parameter, const TalasParameterType& type) noexcept;

// The parameter given a type: an expression its domain and range, others
// as they are, and the type's default field selected: hotValue for a hot type,
// value for another expression's, string (codeString for the codeString type)
// for a text's and symbolList for a choice's; nothing, with error set, for a
// type that does not fit. The field a type selects for its parameter, by its
// name in the Field list.
[[nodiscard]] std::string_view talasParameterTypeField(const TalasParameterType& type);
[[nodiscard]] std::optional<talas::project::ProjectSoundExposedParameter> applyTalasParameterType(
    talas::project::ProjectSoundExposedParameter parameter, const TalasParameterType& type, std::string& error);

// A type defines its parameter's default value, and
// for frequency and duration fields the default units. An expression
// type's default is its legal value nearest 0 (0 where the range holds it),
// written with the units of its domain (hz for the frequency types, samp
// for the duration and time types, whose ranges are in samples), so that a
// bare number in such a field reads in those units; a choice type's is its
// first value, a boolean's false. Text and file types give none.
[[nodiscard]] std::optional<talas::project::ProjectParameterExpression> talasParameterTypeDefault(
    const TalasParameterType& type);
// The parameter's default value as its type has it: kept where the type
// takes it (a constant in its range and domain, read in `language`, an
// Event Value or variable, one of its values), the type's own default
// otherwise. applyTalasParameterType gives a parameter this default as it
// gives the type, reading Val, where a plain number takes the field's
// units.
[[nodiscard]] talas::project::ProjectSoundExposedParameter withTalasParameterTypeDefault(
    talas::project::ProjectSoundExposedParameter parameter, const TalasParameterType& type,
    talas::expr::ExpressionLanguageVersion language = talas::expr::ExpressionLanguageVersion::ValV1);

// Another Sound dragged onto the Parameter list: each of the class's parameters takes the type its
// namesake has in that Sound, its domain, range and choices; one with no
// namesake there, or a namesake of another kind, keeps its own. `sound`
// names a prototype's type, whose parameters' ranges are their namesake
// types', or a Sound definition of the document, a class, whose parameters
// also give their comments and their fields' places and kinds. The ids of the
// parameters changed, in order; nothing for a Sound that is neither.
[[nodiscard]] std::optional<std::vector<std::string>> classParametersTypedAs(
    talas::project::ProjectSoundDefinition& definition, const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry, std::string_view sound);

// Action > Retrieve Example from Class: the Sound the class was made from, its fields'
// ?variables as they were, a plain Sound again (no parameters, class
// description or icon), named after the class with " example" (its id
// the class's with Example) and the first number free when that is taken;
// nothing for no such class.
[[nodiscard]] std::optional<talas::project::ProjectSoundDefinition> classExample(
    const talas::project::ProjectDocument& document, std::string_view classId);

// Retrieve example from class works on a few,
// though not all, of the System classes too. A System class here is one of
// the System prototypes; those built from other Sounds (AR Drag&Drop, a VCA
// whose Gain an AR sets, over a Noise) give the Sounds they are built from,
// as the Prototypes make them, a plain Sound named after the prototype with
// " example" (its id the prototype's with Example) and the first number
// free when that is taken. A System class of one Sound, built in (an
// Oscillator), has no example: nothing, `error` saying so; nothing too,
// with `error` set, for an id that names no System prototype.
[[nodiscard]] std::optional<talas::project::ProjectSoundDefinition> systemClassExample(
    const talas::project::ProjectDocument& document, const talas::core::NodeRegistry& registry,
    std::string_view prototypeId, std::string& error);

// The System class of the Sound selected in the signal flow open (the
// project's when `openDefinitionId` is none): its prototype, when it is a
// Sound of no class of the project's; nothing for a class's instance or no
// such Sound.
[[nodiscard]] std::optional<std::string> systemClassOfSelection(
    const talas::project::ProjectDocument& document,
    const std::optional<std::string>& openDefinitionId,
    std::string_view selectedNodeId);

// Action > Edit Class: the class of the Sound selected, a Sound made from a Sound
// definition, else the Sound definition whose signal flow is open; nothing
// when the selection is a plain Sound of the project's own signal flow.
[[nodiscard]] std::optional<std::string> classOfSelection(
    const talas::project::ProjectDocument& document,
    const std::optional<std::string>& openDefinitionId,
    std::string_view selectedNodeId);

// The first close of the editor on a class New Class from Example made
// asks whether to save an instance
// of it. Yes keeps the class's instances where the example was, each with
// its parameters at their defaults; No starts over, the project as it was
// before the class, in one undoable edit.
[[nodiscard]] std::string newClassCloseQuestion(std::string_view className);
// New Class from Example first asks for the example's free variables'
// values, which become the parameters'
// default values: each parameter standing for a variable given a value
// takes that value; the rest keep theirs.
[[nodiscard]] talas::project::ProjectSoundDefinition classWithDefaults(talas::project::ProjectSoundDefinition definition,
    const std::vector<std::pair<std::string, double>>& values);
[[nodiscard]] std::vector<std::string> newClassInstances(
    const talas::project::ProjectDocument& document, std::string_view classId);

// New Class from Example on a Sound selected in the Sound file window makes the
// class from that Sound, which stays as it is
// (`talas::project::classFromSoundFileExample`).
// Yes on the Class Editor's first close then saves an instance of the class
// in the Sound file window: the project with that Sound,
// named as the class (`talas::project::soundFileClassInstance`), and the
// window's selection, that instance alone, so that it is highlighted.
// Nothing for no such class.
struct SoundFileClassInstanceSaved
{
    talas::project::ProjectDocument document;
    std::vector<std::string> selection;
};
[[nodiscard]] std::optional<SoundFileClassInstanceSaved> classInstanceSavedInSoundFile(
    const talas::project::ProjectDocument& document, std::string_view classId);
}
