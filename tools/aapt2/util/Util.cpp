/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "util/Util.h"

#include <algorithm>
#include <ostream>
#include <string>
#include <vector>

#include "androidfw/StringPiece.h"
#include "utils/Unicode.h"

#include "text/Unicode.h"
#include "text/Utf8Iterator.h"
#include "util/BigBuffer.h"
#include "util/Maybe.h"

using ::aapt::text::Utf8Iterator;
using ::android::StringPiece;
using ::android::StringPiece16;

namespace aapt {
namespace util {

static std::vector<std::string> SplitAndTransform(
    const StringPiece& str, char sep, const std::function<char(char)>& f) {
  std::vector<std::string> parts;
  const StringPiece::const_iterator end = std::end(str);
  StringPiece::const_iterator start = std::begin(str);
  StringPiece::const_iterator current;
  do {
    current = std::find(start, end, sep);
    parts.emplace_back(str.substr(start, current).to_string());
    if (f) {
      std::string& part = parts.back();
      std::transform(part.begin(), part.end(), part.begin(), f);
    }
    start = current + 1;
  } while (current != end);
  return parts;
}

std::vector<std::string> Split(const StringPiece& str, char sep) {
  return SplitAndTransform(str, sep, nullptr);
}

std::vector<std::string> SplitAndLowercase(const StringPiece& str, char sep) {
  return SplitAndTransform(str, sep, ::tolower);
}

bool StartsWith(const StringPiece& str, const StringPiece& prefix) {
  if (str.size() < prefix.size()) {
    return false;
  }
  return str.substr(0, prefix.size()) == prefix;
}

bool EndsWith(const StringPiece& str, const StringPiece& suffix) {
  if (str.size() < suffix.size()) {
    return false;
  }
  return str.substr(str.size() - suffix.size(), suffix.size()) == suffix;
}

StringPiece TrimWhitespace(const StringPiece& str) {
  if (str.size() == 0 || str.data() == nullptr) {
    return str;
  }

  const char* start = str.data();
  const char* end = str.data() + str.length();

  while (start != end && isspace(*start)) {
    start++;
  }

  while (end != start && isspace(*(end - 1))) {
    end--;
  }

  return StringPiece(start, end - start);
}

static int IsJavaNameImpl(const StringPiece& str) {
  int pieces = 0;
  for (const StringPiece& piece : Tokenize(str, '.')) {
    pieces++;
    if (!text::IsJavaIdentifier(piece)) {
      return -1;
    }
  }
  return pieces;
}

bool IsJavaClassName(const StringPiece& str) {
  return IsJavaNameImpl(str) >= 2;
}

bool IsJavaPackageName(const StringPiece& str) {
  return IsJavaNameImpl(str) >= 1;
}

static int IsAndroidNameImpl(const StringPiece& str) {
  int pieces = 0;
  for (const StringPiece& piece : Tokenize(str, '.')) {
    if (piece.empty()) {
      return -1;
    }

    const char first_character = piece.data()[0];
    if (!::isalpha(first_character)) {
      return -1;
    }

    bool valid = std::all_of(piece.begin() + 1, piece.end(), [](const char c) -> bool {
      return ::isalnum(c) || c == '_';
    });

    if (!valid) {
      return -1;
    }
    pieces++;
  }
  return pieces;
}

bool IsAndroidPackageName(const StringPiece& str) {
  return IsAndroidNameImpl(str) > 1 || str == "android";
}

bool IsAndroidSplitName(const StringPiece& str) {
  return IsAndroidNameImpl(str) > 0;
}

Maybe<std::string> GetFullyQualifiedClassName(const StringPiece& package,
                                              const StringPiece& classname) {
  if (classname.empty()) {
    return {};
  }

  if (util::IsJavaClassName(classname)) {
    return classname.to_string();
  }

  if (package.empty()) {
    return {};
  }

  std::string result = package.to_string();
  if (classname.data()[0] != '.') {
    result += '.';
  }

  result.append(classname.data(), classname.size());
  if (!IsJavaClassName(result)) {
    return {};
  }
  return result;
}

static size_t ConsumeDigits(const char* start, const char* end) {
  const char* c = start;
  for (; c != end && *c >= '0' && *c <= '9'; c++) {
  }
  return static_cast<size_t>(c - start);
}

bool VerifyJavaStringFormat(const StringPiece& str) {
  const char* c = str.begin();
  const char* const end = str.end();

  size_t arg_count = 0;
  bool nonpositional = false;
  while (c != end) {
    if (*c == '%' && c + 1 < end) {
      c++;

      if (*c == '%' || *c == 'n') {
        c++;
        continue;
      }

      arg_count++;

      size_t num_digits = ConsumeDigits(c, end);
      if (num_digits > 0) {
        c += num_digits;
        if (c != end && *c != '$') {
          // The digits were a size, but not a positional argument.
          nonpositional = true;
        }
      } else if (*c == '<') {
        // Reusing last argument, bad idea since positions can be moved around
        // during translation.
        nonpositional = true;

        c++;

        // Optionally we can have a $ after
        if (c != end && *c == '$') {
          c++;
        }
      } else {
        nonpositional = true;
      }

      // Ignore size, width, flags, etc.
      while (c != end && (*c == '-' || *c == '#' || *c == '+' || *c == ' ' ||
                          *c == ',' || *c == '(' || (*c >= '0' && *c <= '9'))) {
        c++;
      }

      /*
       * This is a shortcut to detect strings that are going to Time.format()
       * instead of String.format()
       *
       * Comparison of String.format() and Time.format() args:
       *
       * String: ABC E GH  ST X abcdefgh  nost x
       *   Time:    DEFGHKMS W Za  d   hkm  s w yz
       *
       * Therefore we know it's definitely Time if we have:
       *     DFKMWZkmwyz
       */
      if (c != end) {
        switch (*c) {
          case 'D':
          case 'F':
          case 'K':
          case 'M':
          case 'W':
          case 'Z':
          case 'k':
          case 'm':
          case 'w':
          case 'y':
          case 'z':
            return true;
        }
      }
    }

    if (c != end) {
      c++;
    }
  }

  if (arg_count > 1 && nonpositional) {
    // Multiple arguments were specified, but some or all were non positional.
    // Translated
    // strings may rearrange the order of the arguments, which will break the
    // string.
    return false;
  }
  return true;
}

static bool AppendCodepointToUtf8String(char32_t codepoint, std::string* output) {
  ssize_t len = utf32_to_utf8_length(&codepoint, 1);
  if (len < 0) {
    return false;
  }

  const size_t start_append_pos = output->size();

  // Make room for the next character.
  output->resize(output->size() + len);

  char* dst = &*(output->begin() + start_append_pos);
  utf32_to_utf8(&codepoint, 1, dst, len + 1);
  return true;
}

static bool AppendUnicodeCodepoint(Utf8Iterator* iter, std::string* output) {
  char32_t code = 0;
  for (size_t i = 0; i < 4 && iter->HasNext(); i++) {
    char32_t codepoint = iter->Next();
    char32_t a;
    if (codepoint >= U'0' && codepoint <= U'9') {
      a = codepoint - U'0';
    } else if (codepoint >= U'a' && codepoint <= U'f') {
      a = codepoint - U'a' + 10;
    } else if (codepoint >= U'A' && codepoint <= U'F') {
      a = codepoint - U'A' + 10;
    } else {
      return {};
    }
    code = (code << 4) | a;
  }
  return AppendCodepointToUtf8String(code, output);
}

static bool IsCodepointSpace(char32_t codepoint) {
  if (static_cast<uint32_t>(codepoint) & 0xffffff00u) {
    return false;
  }
  return isspace(static_cast<char>(codepoint));
}

StringBuilder::StringBuilder(bool preserve_spaces) : preserve_spaces_(preserve_spaces) {
}

StringBuilder& StringBuilder::Append(const StringPiece& str) {
  if (!error_.empty()) {
    return *this;
  }

  // Where the new data will be appended to.
  const size_t new_data_index = str_.size();

  Utf8Iterator iter(str);
  while (iter.HasNext()) {
    const char32_t codepoint = iter.Next();

    if (last_char_was_escape_) {
      switch (codepoint) {
        case U't':
          str_ += '\t';
          break;

        case U'n':
          str_ += '\n';
          break;

        case U'#':
        case U'@':
        case U'?':
        case U'"':
        case U'\'':
        case U'\\':
          str_ += static_cast<char>(codepoint);
          break;

        case U'u':
          if (!AppendUnicodeCodepoint(&iter, &str_)) {
            error_ = "invalid unicode escape sequence";
            return *this;
          }
          break;

        default:
          // Ignore the escape character and just include the codepoint.
          AppendCodepointToUtf8String(codepoint, &str_);
          break;
      }
      last_char_was_escape_ = false;

    } else if (!preserve_spaces_ && codepoint == U'"') {
      if (!quote_ && trailing_space_) {
        // We found an opening quote, and we have trailing space, so we should append that
        // space now.
        if (trailing_space_) {
          // We had trailing whitespace, so replace with a single space.
          if (!str_.empty()) {
            str_ += ' ';
          }
          trailing_space_ = false;
        }
      }
      quote_ = !quote_;

    } else if (!preserve_spaces_ && codepoint == U'\'' && !quote_) {
      // This should be escaped.
      error_ = "unescaped apostrophe";
      return *this;

    } else if (codepoint == U'\\') {
      // This is an escape sequence, convert to the real value.
      if (!quote_ && trailing_space_) {
        // We had trailing whitespace, so
        // replace with a single space.
        if (!str_.empty()) {
          str_ += ' ';
        }
        trailing_space_ = false;
      }
      last_char_was_escape_ = true;
    } else {
      if (preserve_spaces_ || quote_) {
        // Quotes mean everything is taken, including whitespace.
        AppendCodepointToUtf8String(codepoint, &str_);
      } else {
        // This is not quoted text, so we will accumulate whitespace and only emit a single
        // character of whitespace if it is followed by a non-whitespace character.
        if (IsCodepointSpace(codepoint)) {
          // We found whitespace.
          trailing_space_ = true;
        } else {
          if (trailing_space_) {
            // We saw trailing space before, so replace all
            // that trailing space with one space.
            if (!str_.empty()) {
              str_ += ' ';
            }
            trailing_space_ = false;
          }
          AppendCodepointToUtf8String(codepoint, &str_);
        }
      }
    }
  }

  // Accumulate the added string's UTF-16 length.
  ssize_t len = utf8_to_utf16_length(reinterpret_cast<const uint8_t*>(str_.data()) + new_data_index,
                                     str_.size() - new_data_index);
  if (len < 0) {
    error_ = "invalid unicode code point";
    return *this;
  }
  utf16_len_ += len;
  return *this;
}

std::u16string Utf8ToUtf16(const StringPiece& utf8) {
  ssize_t utf16_length = utf8_to_utf16_length(
      reinterpret_cast<const uint8_t*>(utf8.data()), utf8.length());
  if (utf16_length <= 0) {
    return {};
  }

  std::u16string utf16;
  utf16.resize(utf16_length);
  utf8_to_utf16(reinterpret_cast<const uint8_t*>(utf8.data()), utf8.length(),
                &*utf16.begin(), utf16_length + 1);
  return utf16;
}

std::string Utf16ToUtf8(const StringPiece16& utf16) {
  ssize_t utf8_length = utf16_to_utf8_length(utf16.data(), utf16.length());
  if (utf8_length <= 0) {
    return {};
  }

  std::string utf8;
  utf8.resize(utf8_length);
  utf16_to_utf8(utf16.data(), utf16.length(), &*utf8.begin(), utf8_length + 1);
  return utf8;
}

bool WriteAll(std::ostream& out, const BigBuffer& buffer) {
  for (const auto& b : buffer) {
    if (!out.write(reinterpret_cast<const char*>(b.buffer.get()), b.size)) {
      return false;
    }
  }
  return true;
}

std::unique_ptr<uint8_t[]> Copy(const BigBuffer& buffer) {
  std::unique_ptr<uint8_t[]> data =
      std::unique_ptr<uint8_t[]>(new uint8_t[buffer.size()]);
  uint8_t* p = data.get();
  for (const auto& block : buffer) {
    memcpy(p, block.buffer.get(), block.size);
    p += block.size;
  }
  return data;
}

typename Tokenizer::iterator& Tokenizer::iterator::operator++() {
  const char* start = token_.end();
  const char* end = str_.end();
  if (start == end) {
    end_ = true;
    token_.assign(token_.end(), 0);
    return *this;
  }

  start += 1;
  const char* current = start;
  while (current != end) {
    if (*current == separator_) {
      token_.assign(start, current - start);
      return *this;
    }
    ++current;
  }
  token_.assign(start, end - start);
  return *this;
}

bool Tokenizer::iterator::operator==(const iterator& rhs) const {
  // We check equality here a bit differently.
  // We need to know that the addresses are the same.
  return token_.begin() == rhs.token_.begin() &&
         token_.end() == rhs.token_.end() && end_ == rhs.end_;
}

bool Tokenizer::iterator::operator!=(const iterator& rhs) const {
  return !(*this == rhs);
}

Tokenizer::iterator::iterator(StringPiece s, char sep, StringPiece tok,
                              bool end)
    : str_(s), separator_(sep), token_(tok), end_(end) {}

Tokenizer::Tokenizer(StringPiece str, char sep)
    : begin_(++iterator(str, sep, StringPiece(str.begin() - 1, 0), false)),
      end_(str, sep, StringPiece(str.end(), 0), true) {}

bool ExtractResFilePathParts(const StringPiece& path, StringPiece* out_prefix,
                             StringPiece* out_entry, StringPiece* out_suffix) {
  const StringPiece res_prefix("res/");
  if (!StartsWith(path, res_prefix)) {
    return false;
  }

  StringPiece::const_iterator last_occurence = path.end();
  for (auto iter = path.begin() + res_prefix.size(); iter != path.end();
       ++iter) {
    if (*iter == '/') {
      last_occurence = iter;
    }
  }

  if (last_occurence == path.end()) {
    return false;
  }

  auto iter = std::find(last_occurence, path.end(), '.');
  *out_suffix = StringPiece(iter, path.end() - iter);
  *out_entry = StringPiece(last_occurence + 1, iter - last_occurence - 1);
  *out_prefix = StringPiece(path.begin(), last_occurence - path.begin() + 1);
  return true;
}

StringPiece16 GetString16(const android::ResStringPool& pool, size_t idx) {
  size_t len;
  const char16_t* str = pool.stringAt(idx, &len);
  if (str != nullptr) {
    return StringPiece16(str, len);
  }
  return StringPiece16();
}

std::string GetString(const android::ResStringPool& pool, size_t idx) {
  size_t len;
  const char* str = pool.string8At(idx, &len);
  if (str != nullptr) {
    return std::string(str, len);
  }
  return Utf16ToUtf8(GetString16(pool, idx));
}

}  // namespace util
}  // namespace aapt
