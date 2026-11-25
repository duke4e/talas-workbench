#pragma once

#include <filesystem>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::app
{
// A library of commonly used expressions to
// drag into parameter fields, each with a comment on what it does and
// where it fits. Talas's own entries, in Val (the ValV1
// expression language), grouped by kind: arrays,
// tempo, time-varying, fader arithmetic, fader warping, MIDI, random
// values, memory, reserved words, sequences, spectra, time expansion and
// compression, triggers and unit conversions.
struct ExpressionLibraryEntry
{
    std::string category;
    std::string name;
    std::string comment;
    std::string expression;
};

[[nodiscard]] std::span<const ExpressionLibraryEntry> expressionsLibrary();

// The entries whose category, name, comment or expression holds every word
// of the query, ignoring case; all of them for an empty query.
[[nodiscard]] std::vector<ExpressionLibraryEntry> findExpressionLibraryEntries(std::string_view query);

// An expressions file of one's own: a text file
// beginning with EXPR on a line by itself, then for each expression its
// name after a double quote to the end of the line, its comment on the
// lines after up to a closing double quote, and the expression itself on
// the lines after that; `category` the file's. Nothing for a file that does
// not begin with EXPR; an entry with no expression is left out.
[[nodiscard]] std::optional<std::vector<ExpressionLibraryEntry>> parseExpressionsFile(
    std::string_view text, std::string category);
// The expressions files among the .txt files of the folders given and of an
// Expressions Library folder in each, by name, each file's name its
// category; files that are not expressions files are passed over.
[[nodiscard]] std::vector<ExpressionLibraryEntry> expressionsFilesIn(const std::vector<std::filesystem::path>& folders);
// The search over the library's own entries and these too.
[[nodiscard]] std::vector<ExpressionLibraryEntry> findExpressionLibraryEntries(
    std::string_view query, std::span<const ExpressionLibraryEntry> more);

// The text a drag from the library carries, and the expression it holds.
[[nodiscard]] std::string expressionLibraryDragDescription(const ExpressionLibraryEntry& entry);
[[nodiscard]] std::optional<std::string> expressionFromLibraryDrag(std::string_view description);

// The Expressions Library as text files: the library's expressions, each kind
// in a file, together with the files and expressions a tutorial opens by name
// (keyboard expressions.txt's Pitch plus an interval, fader expressions.txt's
// Fader inverted (max=10), BPM.txt's Loop a sample at any !BPM, and others),
// in Talas's own Val and words.
struct ExpressionsLibraryFile
{
    std::string name;
    std::vector<ExpressionLibraryEntry> entries;
};

[[nodiscard]] std::vector<ExpressionsLibraryFile> expressionsLibraryFiles();
// An expressions file's text, as parseExpressionsFile reads it back.
[[nodiscard]] std::string expressionsFileText(std::span<const ExpressionLibraryEntry> entries);
// The library's files written into a folder, any already there left as
// they are, so one's own changes to them stay; those written.
std::vector<std::filesystem::path> writeExpressionsLibrary(const std::filesystem::path& folder);

// A field's text after an expression is dropped on it: as a paste, it takes
// the place of the text selected, from `start` to `end`; with nothing
// selected, of the whole text.
[[nodiscard]] std::string fieldTextAfterExpressionDrop(
    std::string_view text, std::size_t start, std::size_t end, std::string_view expression);
}
