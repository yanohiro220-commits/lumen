#include "helpers.hpp"

#include "lumen/lexer.hpp"

using namespace lumen;

namespace {

std::vector<Token> scan(const std::string& src) {
  Lexer lexer(src);
  return lexer.scan();
}

std::vector<TokenType> types(const std::string& src) {
  std::vector<TokenType> out;
  for (const Token& t : scan(src)) out.push_back(t.type);
  return out;
}

}  // namespace

TEST(Lexer, EmptyInputIsJustEof) {
  const auto t = types("");
  CHECK_EQ(t.size(), std::size_t{1});
  CHECK(t[0] == TokenType::Eof);
}

TEST(Lexer, WhitespaceAndCommentsAreSkipped) {
  const auto t = types("  \t\n # a comment\n  42 # trailing\n");
  CHECK_EQ(t.size(), std::size_t{2});
  CHECK(t[0] == TokenType::Number);
  CHECK(t[1] == TokenType::Eof);
}

TEST(Lexer, SingleAndDoubleCharacterOperators) {
  const auto t = types("! != = == < <= > >=");
  const std::vector<TokenType> want = {
      TokenType::Bang, TokenType::BangEqual, TokenType::Equal,
      TokenType::EqualEqual, TokenType::Less, TokenType::LessEqual,
      TokenType::Greater, TokenType::GreaterEqual, TokenType::Eof};
  CHECK_EQ(t.size(), want.size());
  for (std::size_t i = 0; i < want.size(); ++i) CHECK(t[i] == want[i]);
}

TEST(Lexer, KeywordsAreNotIdentifiers) {
  const auto t = types("let x fn if else while for return true false nil and or print break continue");
  const std::vector<TokenType> want = {
      TokenType::Let,    TokenType::Identifier, TokenType::Fn,
      TokenType::If,     TokenType::Else,       TokenType::While,
      TokenType::For,    TokenType::Return,     TokenType::True,
      TokenType::False,  TokenType::Nil,        TokenType::And,
      TokenType::Or,     TokenType::Print,      TokenType::Break,
      TokenType::Continue, TokenType::Eof};
  CHECK_EQ(t.size(), want.size());
  for (std::size_t i = 0; i < want.size(); ++i) CHECK(t[i] == want[i]);
}

TEST(Lexer, IdentifiersThatStartWithAKeyword) {
  // `iffy` is an identifier, not `if` followed by `fy`. A lexer that matches
  // keywords by prefix gets this wrong and the error surfaces as a bizarre
  // parse failure far from the cause.
  const auto t = types("iffy letter fnord");
  CHECK(t[0] == TokenType::Identifier);
  CHECK(t[1] == TokenType::Identifier);
  CHECK(t[2] == TokenType::Identifier);
}

TEST(Lexer, NumberForms) {
  // The source has to outlive the tokens: Token::text is a view into it, and
  // scanning a temporary leaves every view dangling the moment the statement
  // ends.
  const std::string src = "1 42 3.14 0.5 1e9 2E-3 1.5e+2";
  const auto tokens = scan(src);
  CHECK_EQ(tokens.size(), std::size_t{8});
  for (std::size_t i = 0; i < 7; ++i) {
    CHECK(tokens[i].type == TokenType::Number);
  }
  CHECK_EQ(std::string(tokens[4].text), std::string("1e9"));
  CHECK_EQ(std::string(tokens[5].text), std::string("2E-3"));
  CHECK_EQ(std::string(tokens[6].text), std::string("1.5e+2"));
}

TEST(Lexer, TrailingDotIsNotPartOfANumber) {
  // `1.` would be ambiguous with a future member access, so the dot is its own
  // token and `1` ends there.
  const std::string src = "1.foo";
  const auto tokens = scan(src);
  CHECK(tokens[0].type == TokenType::Number);
  CHECK_EQ(std::string(tokens[0].text), std::string("1"));
  CHECK(tokens[1].type == TokenType::Dot);
}

TEST(Lexer, BareExponentLetterIsAnIdentifier) {
  // `1e` is not a valid exponent, so it must lex as `1` then `e`.
  const std::string src = "1e";
  const auto tokens = scan(src);
  CHECK(tokens[0].type == TokenType::Number);
  CHECK_EQ(std::string(tokens[0].text), std::string("1"));
  CHECK(tokens[1].type == TokenType::Identifier);
}

TEST(Lexer, StringEscapes) {
  const auto tokens = scan(R"("a\nb\tc\\d\"e")");
  CHECK(tokens[0].type == TokenType::String);
  CHECK_EQ(tokens[0].literal, std::string("a\nb\tc\\d\"e"));
}

TEST(Lexer, EmptyString) {
  const auto tokens = scan("\"\"");
  CHECK(tokens[0].type == TokenType::String);
  CHECK(tokens[0].literal.empty());
}

TEST(Lexer, UnterminatedStringReportsTheOpeningLine) {
  Lexer lexer("let a = 1;\nlet s = \"oops\nlet b = 2;");
  const auto tokens = lexer.scan();
  CHECK(lexer.had_error());
  bool found = false;
  for (const Token& t : tokens) {
    if (t.type == TokenType::Error) {
      found = true;
      CHECK(lumen_test::contains(t.literal, "unterminated string"));
      CHECK(lumen_test::contains(t.literal, "line 2"));
    }
  }
  CHECK(found);
}

TEST(Lexer, UnknownEscapeIsReported) {
  Lexer lexer(R"("bad \q escape")");
  lexer.scan();
  CHECK(lexer.had_error());
}

TEST(Lexer, UnexpectedCharacterIsReported) {
  Lexer lexer("let a = 1 @ 2;");
  const auto tokens = lexer.scan();
  CHECK(lexer.had_error());
  bool found = false;
  for (const Token& t : tokens) {
    if (t.type == TokenType::Error) {
      found = true;
      CHECK(lumen_test::contains(t.literal, "unexpected character"));
    }
  }
  CHECK(found);
}

TEST(Lexer, LineNumbersTrackNewlines) {
  const auto tokens = scan("a\nb\n\nc");
  CHECK_EQ(tokens[0].line, 1);
  CHECK_EQ(tokens[1].line, 2);
  CHECK_EQ(tokens[2].line, 4);
}

TEST(Lexer, CommentAtEndOfFileWithoutNewline) {
  const auto t = types("42 # no newline after this");
  CHECK_EQ(t.size(), std::size_t{2});
  CHECK(t[0] == TokenType::Number);
}

TEST(Lexer, BracketsAndPunctuation) {
  const auto t = types("()[]{},.;:+-*/%");
  const std::vector<TokenType> want = {
      TokenType::LeftParen, TokenType::RightParen, TokenType::LeftBracket,
      TokenType::RightBracket, TokenType::LeftBrace, TokenType::RightBrace,
      TokenType::Comma, TokenType::Dot, TokenType::Semicolon, TokenType::Colon,
      TokenType::Plus, TokenType::Minus, TokenType::Star, TokenType::Slash,
      TokenType::Percent, TokenType::Eof};
  CHECK_EQ(t.size(), want.size());
  for (std::size_t i = 0; i < want.size(); ++i) CHECK(t[i] == want[i]);
}
