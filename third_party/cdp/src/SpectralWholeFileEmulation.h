/*
 * Derived from the CDP System, Release 8: dev/cdp2k/readdata.c, tklib1.c,
 * tklib3.c and writedata.c, dev/cdparse/cdparse.c and dev/newsfsys/snd.c
 * Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#pragma once

// The pieces of CDP's spectral-program runtime the whole-file ports share
// (SpectralWholeFile.h): the .ana file as the programs see it, the memory
// their buffers live in, the files they read, seek and write, and how they
// read their parameters. Each program is a transliteration of CDP's code
// over these, so that its buffer arithmetic, including its bugs, gives
// CDP's output; where CDP would read memory it never wrote, seek past its
// file or move a negative count, the port refuses instead.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::cdp::wholefile
{
// A refusal: CDP's message (or ours, where CDP's behaviour is undefined).
struct Refusal
{
    std::string message;
};

[[noreturn]] void refuse(std::string message);

// An analysis file as cdparse.c describes it to the programs: wanted floats
// a window (CDP's channels, points + 2), wlength windows, the header's
// arate and the frametime and duration derived from it.
struct AnaFile
{
    std::vector<float> data;
    int wanted = 0;
    int wlength = 0;
    float arate = 0.0F;
    // (float)(1.0 / arate).
    float frametime = 0.0F;
    // wlength * frametime, computed in float as cdparse.c computes it.
    double duration = 0.0;
    // origrate / 2.0.
    double nyquist = 0.0;
    [[nodiscard]] int insams() const noexcept { return static_cast<int>(data.size()); }
};

// One malloc'd block: its floats, and whether each was ever written (a
// malloc'd block starts undefined). A pointer into it is (block, index);
// CDP compares pointers just outside a block, so indices are signed and
// only reads and writes are bounds-checked.
struct Block
{
    std::vector<float> values;
    std::vector<std::uint8_t> defined;
    explicit Block(std::size_t size) : values(size, 0.0F), defined(size, 0) {}
    [[nodiscard]] long size() const noexcept { return static_cast<long>(values.size()); }
};

struct Ptr
{
    Block* block = nullptr;
    long index = 0;
    [[nodiscard]] Ptr operator+(long offset) const noexcept { return {block, index + offset}; }
    [[nodiscard]] Ptr operator-(long offset) const noexcept { return {block, index - offset}; }
    Ptr& operator+=(long offset) noexcept
    {
        index += offset;
        return *this;
    }
    Ptr& operator-=(long offset) noexcept
    {
        index -= offset;
        return *this;
    }
    [[nodiscard]] long operator-(const Ptr& other) const noexcept { return index - other.index; }
    [[nodiscard]] bool operator<(const Ptr& other) const noexcept { return index < other.index; }
    [[nodiscard]] bool operator>(const Ptr& other) const noexcept { return index > other.index; }
    [[nodiscard]] bool operator<=(const Ptr& other) const noexcept { return index <= other.index; }
    [[nodiscard]] bool operator>=(const Ptr& other) const noexcept { return index >= other.index; }
    [[nodiscard]] bool operator==(const Ptr& other) const noexcept
    {
        return block == other.block && index == other.index;
    }
    // A read, refused outside the block; isDefined says whether the float
    // was ever written, which a computed value inherits from its operands.
    [[nodiscard]] float get(long offset = 0) const;
    [[nodiscard]] bool isDefined(long offset = 0) const;
    void set(long offset, float value, bool isDefinedValue = true) const;
};

// memmove(dst, src, count floats): refused for a negative count (CDP casts
// it to a huge size_t) or outside either block.
void move(Ptr dst, Ptr src, long count);

// An input file opened by sndopenEx: fgetfbufEx reads floats from the
// current position; sndseekEx moves it (whence 0 from the start, 1 from
// the position), clamps it at 0 and gives the new position. A position
// past the file's end is refused (CDP's sndseekEx leaves a negative
// remaining count that fgetfbufEx then copies).
class InFile
{
public:
    explicit InFile(const AnaFile& file) noexcept : file_(&file) {}
    int read(Ptr dst, int count);
    int seek(long distance, int whence);
    [[nodiscard]] long tell() const noexcept { return position_; }

private:
    const AnaFile* file_;
    long position_ = 0;
};

// The output file: write_samps and write_exact_samps refuse a count of 0
// or less (fputfbufEx writes nothing and CDP stops with "Can't write to
// output soundfile"), and the port refuses floats CDP never wrote.
class OutFile
{
public:
    void write(Ptr src, long count);
    [[nodiscard]] std::vector<float>& data() noexcept { return data_; }

private:
    std::vector<float> data_;
};

// CDP's flteq and FLTERR.
inline constexpr double FLTERR = 0.000002;
[[nodiscard]] bool flteq(double f1, double f2) noexcept;
// CDP's round (lround), refused where lround is undefined.
[[nodiscard]] int cdpRound(double value);
// (int) of a double, refused where the conversion is undefined.
[[nodiscard]] int truncateToInt(double value);

// Field texts, by field id, with the defaults applied.
using FieldTexts = std::map<std::string, std::string, std::less<>>;

// The field's label for messages, e.g. "Stretch Time's stretch".
struct Field
{
    std::string_view process;
    std::string_view id;
    [[nodiscard]] std::string name() const;
};

// value_is_numeric (tklib1.c).
[[nodiscard]] bool valueIsNumeric(std::string_view text) noexcept;

// read_param_as_value_or_brkfile_and_check_range for a value: a numeric
// text, read as sscanf's %lf, snapped to a limit within FLTERR and refused
// outside lo to hi.
[[nodiscard]] double readNumber(const Field& field, std::string_view text, double lo, double hi);
// And for an "i" parameter, rounded as CDP rounds it.
[[nodiscard]] int readInteger(const Field& field, std::string_view text, double lo, double hi);

// get_float_from_within_string: the next number in text from position, or
// nothing (position unchanged) where CDP's reading stops.
[[nodiscard]] std::optional<double> getFloatFromWithinString(std::string_view text, std::size_t& position);

// fgets(temp, 200, fp) over a text file holding text (with a final
// newline, as the fixtures' data files have): each piece CDP reads.
[[nodiscard]] std::vector<std::string> fgetsPieces(std::string_view text);

// get_brkpnt_data_from_file_and_test_it: a breakpoint table's (time,
// value) pairs, flattened, for a parameter ranging lo to hi.
[[nodiscard]] std::vector<double> readBreakpointTable(const Field& field, std::string_view text, double lo, double hi);

// The programs. Each takes the analysis files and the fields' texts and
// gives the output .ana data (whole windows), refusing as CDP refuses.
[[nodiscard]] std::vector<float> stretchTime(std::span<const AnaFile> in, const FieldTexts& fields);
[[nodiscard]] std::vector<float> specCut(std::span<const AnaFile> in, const FieldTexts& fields);
[[nodiscard]] std::vector<float> specGrab(std::span<const AnaFile> in, const FieldTexts& fields);
[[nodiscard]] std::vector<float> specMagnify(std::span<const AnaFile> in, const FieldTexts& fields);
[[nodiscard]] std::vector<float> focusHold(std::span<const AnaFile> in, const FieldTexts& fields);
[[nodiscard]] std::vector<float> combineInterleave(std::span<const AnaFile> in, const FieldTexts& fields);
[[nodiscard]] std::vector<float> blurDrunk(std::span<const AnaFile> in, const FieldTexts& fields);
[[nodiscard]] std::vector<float> blurShuffle(std::span<const AnaFile> in, const FieldTexts& fields);
[[nodiscard]] std::vector<float> blurWeave(std::span<const AnaFile> in, const FieldTexts& fields);

// CDP's BUF_MULTIPLIER: the programs' buffers hold this many windows.
inline constexpr int BUF_MULTIPLIER = 20;
// The most output floats a program may write before the port refuses (CDP
// would write them; the port keeps the whole file in memory).
inline constexpr std::size_t maximumOutputFloats = std::size_t{1} << 27;

[[nodiscard]] const std::string& fieldText(const FieldTexts& fields, std::string_view id);
}
