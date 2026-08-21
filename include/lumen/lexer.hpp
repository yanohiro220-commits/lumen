#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lumen {

enum class TokenType : std::uint8_t {
  // Single character
  LeftParen, RightParen, LeftBrace, RightBrace, LeftBracket, RightBracket,
  Comma, Dot, Minus, Plus, Semicolon, Slash, Star, Percent, Colon,
  // One or two characters
  Bang, BangEqual, Equal, EqualEqual, Greater, GreaterEqual, Less, LessEqual,
  // Literals
  Identifier, String, Number,
  // Keywords
  And, Else, False, Fn, For, If, Let, Nil, Or, Return, True, While, Print,
  Break, Continue,
  // Meta
  Error, Eof,
};

const char* token_type_name(TokenType t);

struct Token {
  TokenType type = TokenType::Eof;
  std::string_view text;  // a view into the source, which outlives the tokens
  int line = 1;
  int column = 1;

  // Populated for String tokens: the text with escapes resolved. Kept
  // separate so the token still points at the original span for error
  // reporting.
  std::string literal;
};

// Hand-written lexer.
//
// Hand-written rather than generated for the usual reason: error messages. A
// generated scanner reports "unexpected character"; this one can say which
// string was left unterminated and on which line it started, and that is most
// of what makes a language usable.
class Lexer {
 public:
  explicit Lexer(std::string_view source) : src_(source) {}

  // Scans the whole input. The token stream always ends with Eof, and a
  // malformed input produces an Error token carrying the message rather than
  // throwing, so the parser can report several problems in one run.
  std::vector<Token> scan();

  bool had_error() const { return had_error_; }

 private:
  char peek() const { return at_end() ? '\0' : src_[pos_]; }
  char peek_next() const { return pos_ + 1 >= src_.size() ? '\0' : src_[pos_ + 1]; }
  bool at_end() const { return pos_ >= src_.size(); }
  char advance();
  bool match(char expected);
  void skip_whitespace();

  Token make(TokenType type) const;
  Token error(std::string message);
  Token string_token();
  Token number_token();
  Token identifier_token();

  std::string_view src_;
  std::size_t pos_ = 0;
  std::size_t start_ = 0;
  int line_ = 1;
  int line_start_ = 0;
  bool had_error_ = false;
  std::vector<std::string> messages_;
};

}  // namespace lumen
