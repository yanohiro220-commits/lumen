#include "lumen/lexer.hpp"

#include <cctype>
#include <unordered_map>

namespace lumen {

const char* token_type_name(TokenType t) {
  switch (t) {
    case TokenType::LeftParen: return "(";
    case TokenType::RightParen: return ")";
    case TokenType::LeftBrace: return "{";
    case TokenType::RightBrace: return "}";
    case TokenType::LeftBracket: return "[";
    case TokenType::RightBracket: return "]";
    case TokenType::Comma: return ",";
    case TokenType::Dot: return ".";
    case TokenType::Minus: return "-";
    case TokenType::Plus: return "+";
    case TokenType::Semicolon: return ";";
    case TokenType::Slash: return "/";
    case TokenType::Star: return "*";
    case TokenType::Percent: return "%";
    case TokenType::Colon: return ":";
    case TokenType::Bang: return "!";
    case TokenType::BangEqual: return "!=";
    case TokenType::Equal: return "=";
    case TokenType::EqualEqual: return "==";
    case TokenType::Greater: return ">";
    case TokenType::GreaterEqual: return ">=";
    case TokenType::Less: return "<";
    case TokenType::LessEqual: return "<=";
    case TokenType::Identifier: return "identifier";
    case TokenType::String: return "string";
    case TokenType::Number: return "number";
    case TokenType::And: return "and";
    case TokenType::Else: return "else";
    case TokenType::False: return "false";
    case TokenType::Fn: return "fn";
    case TokenType::For: return "for";
    case TokenType::If: return "if";
    case TokenType::Let: return "let";
    case TokenType::Nil: return "nil";
    case TokenType::Or: return "or";
    case TokenType::Return: return "return";
    case TokenType::True: return "true";
    case TokenType::While: return "while";
    case TokenType::Print: return "print";
    case TokenType::Break: return "break";
    case TokenType::Continue: return "continue";
    case TokenType::Error: return "error";
    case TokenType::Eof: return "end of file";
  }
  return "?";
}

namespace {

const std::unordered_map<std::string_view, TokenType>& keywords() {
  static const std::unordered_map<std::string_view, TokenType> table = {
      {"and", TokenType::And},       {"else", TokenType::Else},
      {"false", TokenType::False},   {"fn", TokenType::Fn},
      {"for", TokenType::For},       {"if", TokenType::If},
      {"let", TokenType::Let},       {"nil", TokenType::Nil},
      {"or", TokenType::Or},         {"return", TokenType::Return},
      {"true", TokenType::True},     {"while", TokenType::While},
      {"print", TokenType::Print},   {"break", TokenType::Break},
      {"continue", TokenType::Continue},
  };
  return table;
}

bool is_ident_start(char c) {
  return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}
bool is_ident_part(char c) {
  return is_ident_start(c) || std::isdigit(static_cast<unsigned char>(c));
}

}  // namespace

char Lexer::advance() {
  char c = src_[pos_++];
  if (c == '\n') {
    ++line_;
    line_start_ = static_cast<int>(pos_);
  }
  return c;
}

bool Lexer::match(char expected) {
  if (at_end() || src_[pos_] != expected) return false;
  ++pos_;
  return true;
}

Token Lexer::make(TokenType type) const {
  Token t;
  t.type = type;
  t.text = src_.substr(start_, pos_ - start_);
  t.line = line_;
  t.column = static_cast<int>(start_) - line_start_ + 1;
  return t;
}

Token Lexer::error(std::string message) {
  had_error_ = true;
  Token t = make(TokenType::Error);
  t.literal = std::move(message);
  return t;
}

void Lexer::skip_whitespace() {
  for (;;) {
    switch (peek()) {
      case ' ':
      case '\r':
      case '\t':
      case '\n':
        advance();
        break;
      case '#':
        // Comment to end of line. Not consumed as a newline, so the line
        // counter stays correct when the loop comes back around.
        while (!at_end() && peek() != '\n') advance();
        break;
      default:
        return;
    }
  }
}

Token Lexer::string_token() {
  const int opening_line = line_;
  std::string out;
  while (!at_end() && peek() != '"') {
    char c = advance();
    if (c != '\\') {
      out.push_back(c);
      continue;
    }
    if (at_end()) break;
    char esc = advance();
    switch (esc) {
      case 'n': out.push_back('\n'); break;
      case 't': out.push_back('\t'); break;
      case 'r': out.push_back('\r'); break;
      case '0': out.push_back('\0'); break;
      case '\\': out.push_back('\\'); break;
      case '"': out.push_back('"'); break;
      default:
        return error(std::string("unknown escape sequence '\\") + esc + "'");
    }
  }
  if (at_end()) {
    return error("unterminated string starting on line " +
                 std::to_string(opening_line));
  }
  advance();  // closing quote
  Token t = make(TokenType::String);
  t.literal = std::move(out);
  return t;
}

Token Lexer::number_token() {
  while (std::isdigit(static_cast<unsigned char>(peek()))) advance();
  if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek_next()))) {
    advance();
    while (std::isdigit(static_cast<unsigned char>(peek()))) advance();
  }
  // Exponent form, so 1e9 does not lex as `1` followed by the identifier `e9`.
  if (peek() == 'e' || peek() == 'E') {
    const std::size_t save = pos_;
    advance();
    if (peek() == '+' || peek() == '-') advance();
    if (std::isdigit(static_cast<unsigned char>(peek()))) {
      while (std::isdigit(static_cast<unsigned char>(peek()))) advance();
    } else {
      pos_ = save;
    }
  }
  return make(TokenType::Number);
}

Token Lexer::identifier_token() {
  while (is_ident_part(peek())) advance();
  const std::string_view text = src_.substr(start_, pos_ - start_);
  const auto it = keywords().find(text);
  return make(it == keywords().end() ? TokenType::Identifier : it->second);
}

std::vector<Token> Lexer::scan() {
  std::vector<Token> tokens;
  for (;;) {
    skip_whitespace();
    start_ = pos_;
    if (at_end()) {
      tokens.push_back(make(TokenType::Eof));
      return tokens;
    }

    const char c = advance();
    if (is_ident_start(c)) {
      tokens.push_back(identifier_token());
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c))) {
      tokens.push_back(number_token());
      continue;
    }

    switch (c) {
      case '(': tokens.push_back(make(TokenType::LeftParen)); break;
      case ')': tokens.push_back(make(TokenType::RightParen)); break;
      case '{': tokens.push_back(make(TokenType::LeftBrace)); break;
      case '}': tokens.push_back(make(TokenType::RightBrace)); break;
      case '[': tokens.push_back(make(TokenType::LeftBracket)); break;
      case ']': tokens.push_back(make(TokenType::RightBracket)); break;
      case ',': tokens.push_back(make(TokenType::Comma)); break;
      case '.': tokens.push_back(make(TokenType::Dot)); break;
      case '-': tokens.push_back(make(TokenType::Minus)); break;
      case '+': tokens.push_back(make(TokenType::Plus)); break;
      case ';': tokens.push_back(make(TokenType::Semicolon)); break;
      case '*': tokens.push_back(make(TokenType::Star)); break;
      case '%': tokens.push_back(make(TokenType::Percent)); break;
      case ':': tokens.push_back(make(TokenType::Colon)); break;
      case '/': tokens.push_back(make(TokenType::Slash)); break;
      case '!':
        tokens.push_back(make(match('=') ? TokenType::BangEqual : TokenType::Bang));
        break;
      case '=':
        tokens.push_back(make(match('=') ? TokenType::EqualEqual : TokenType::Equal));
        break;
      case '<':
        tokens.push_back(make(match('=') ? TokenType::LessEqual : TokenType::Less));
        break;
      case '>':
        tokens.push_back(
            make(match('=') ? TokenType::GreaterEqual : TokenType::Greater));
        break;
      case '"':
        tokens.push_back(string_token());
        break;
      default:
        tokens.push_back(error(std::string("unexpected character '") + c + "'"));
        break;
    }
  }
}

}  // namespace lumen
