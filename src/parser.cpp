#include "lumen/parser.hpp"

#include <charconv>
#include <cstdlib>

namespace lumen {

std::string Diagnostic::format() const {
  return "line " + std::to_string(line) + ":" + std::to_string(column) + ": " +
         message;
}

std::string Literal::to_string() const {
  switch (type) {
    case Type::Nil: return "nil";
    case Type::Bool: return boolean ? "true" : "false";
    case Type::Number: {
      // Integral doubles print without a decimal point, which is what a user
      // means by `print 1 + 1`.
      if (number == static_cast<long long>(number)) {
        return std::to_string(static_cast<long long>(number));
      }
      std::string s = std::to_string(number);
      while (s.size() > 1 && s.back() == '0') s.pop_back();
      if (!s.empty() && s.back() == '.') s.pop_back();
      return s;
    }
    case Type::String: return string;
  }
  return "?";
}

Parser::Precedence Parser::precedence_of(TokenType t) {
  switch (t) {
    case TokenType::Equal: return PrecAssignment;
    case TokenType::Or: return PrecOr;
    case TokenType::And: return PrecAnd;
    case TokenType::EqualEqual:
    case TokenType::BangEqual: return PrecEquality;
    case TokenType::Less:
    case TokenType::LessEqual:
    case TokenType::Greater:
    case TokenType::GreaterEqual: return PrecComparison;
    case TokenType::Plus:
    case TokenType::Minus: return PrecTerm;
    case TokenType::Star:
    case TokenType::Slash:
    case TokenType::Percent: return PrecFactor;
    case TokenType::LeftParen:
    case TokenType::LeftBracket: return PrecCall;
    default: return PrecNone;
  }
}

const Token& Parser::advance() {
  if (!at_end()) ++pos_;
  return previous();
}

bool Parser::match(TokenType t) {
  if (!check(t)) return false;
  advance();
  return true;
}

const Token& Parser::consume(TokenType t, const std::string& message) {
  if (check(t)) return advance();
  error_at(peek(), message);
  return peek();
}

void Parser::error_at(const Token& token, const std::string& message) {
  // Panic mode: after one error, further errors on the way to the next
  // statement boundary are almost always consequences of the first, and
  // reporting them buries the real one.
  if (panic_) return;
  panic_ = true;
  std::string detail = message;
  if (token.type == TokenType::Error) {
    detail = token.literal;
  } else if (token.type != TokenType::Eof) {
    detail += " (found '" + std::string(token.text) + "')";
  } else {
    detail += " (found end of file)";
  }
  errors_.push_back(Diagnostic{token.line, token.column, detail});
}

void Parser::synchronize() {
  panic_ = false;
  while (!at_end()) {
    if (previous().type == TokenType::Semicolon) return;
    switch (peek().type) {
      case TokenType::Fn:
      case TokenType::Let:
      case TokenType::For:
      case TokenType::If:
      case TokenType::While:
      case TokenType::Print:
      case TokenType::Return:
        return;
      default:
        advance();
    }
  }
}

Program Parser::parse() {
  Program program;
  while (!at_end()) {
    if (auto stmt = declaration()) {
      program.statements.push_back(std::move(stmt));
    }
    if (panic_) synchronize();
  }
  return program;
}

StmtPtr Parser::declaration() {
  if (match(TokenType::Let)) return let_declaration();
  if (check(TokenType::Fn) && tokens_[pos_ + 1].type == TokenType::Identifier) {
    advance();
    return function_declaration();
  }
  return statement();
}

StmtPtr Parser::let_declaration() {
  const int line = previous().line;
  const Token& name = consume(TokenType::Identifier, "expected a variable name");
  if (panic_) return nullptr;
  std::string ident(name.text);

  ExprPtr init;
  if (match(TokenType::Equal)) {
    init = expression();
  }
  consume(TokenType::Semicolon, "expected ';' after a let declaration");
  return std::make_unique<LetStmt>(std::move(ident), std::move(init), line);
}

std::shared_ptr<FunctionBody> Parser::function_body(const std::string& name) {
  auto fn = std::make_shared<FunctionBody>();
  fn->name = name;
  fn->line = previous().line;

  consume(TokenType::LeftParen, "expected '(' after a function name");
  if (!check(TokenType::RightParen)) {
    do {
      const Token& param = consume(TokenType::Identifier, "expected a parameter name");
      if (panic_) return fn;
      if (fn->params.size() >= 255) {
        error_at(param, "a function cannot have more than 255 parameters");
        return fn;
      }
      fn->params.emplace_back(param.text);
    } while (match(TokenType::Comma));
  }
  consume(TokenType::RightParen, "expected ')' after parameters");
  consume(TokenType::LeftBrace, "expected '{' before a function body");

  while (!check(TokenType::RightBrace) && !at_end()) {
    if (auto s = declaration()) fn->body.push_back(std::move(s));
    if (panic_) synchronize();
  }
  consume(TokenType::RightBrace, "expected '}' after a function body");
  return fn;
}

StmtPtr Parser::function_declaration() {
  const Token& name = consume(TokenType::Identifier, "expected a function name");
  if (panic_) return nullptr;
  const int line = name.line;
  auto body = function_body(std::string(name.text));
  return std::make_unique<FunctionStmt>(std::move(body), line);
}

StmtPtr Parser::statement() {
  if (match(TokenType::Print)) return print_statement();
  if (match(TokenType::LeftBrace)) return block_statement();
  if (match(TokenType::If)) return if_statement();
  if (match(TokenType::While)) return while_statement();
  if (match(TokenType::For)) return for_statement();
  if (match(TokenType::Return)) return return_statement();
  if (match(TokenType::Break)) {
    const int line = previous().line;
    consume(TokenType::Semicolon, "expected ';' after 'break'");
    return std::make_unique<BreakStmt>(line);
  }
  if (match(TokenType::Continue)) {
    const int line = previous().line;
    consume(TokenType::Semicolon, "expected ';' after 'continue'");
    return std::make_unique<ContinueStmt>(line);
  }
  return expression_statement();
}

StmtPtr Parser::block_statement() {
  const int line = previous().line;
  std::vector<StmtPtr> statements;
  while (!check(TokenType::RightBrace) && !at_end()) {
    if (auto s = declaration()) statements.push_back(std::move(s));
    if (panic_) synchronize();
  }
  consume(TokenType::RightBrace, "expected '}' after a block");
  return std::make_unique<BlockStmt>(std::move(statements), line);
}

StmtPtr Parser::if_statement() {
  const int line = previous().line;
  consume(TokenType::LeftParen, "expected '(' after 'if'");
  ExprPtr cond = expression();
  consume(TokenType::RightParen, "expected ')' after an if condition");
  StmtPtr then_branch = statement();
  StmtPtr else_branch;
  if (match(TokenType::Else)) else_branch = statement();
  return std::make_unique<IfStmt>(std::move(cond), std::move(then_branch),
                                  std::move(else_branch), line);
}

StmtPtr Parser::while_statement() {
  const int line = previous().line;
  consume(TokenType::LeftParen, "expected '(' after 'while'");
  ExprPtr cond = expression();
  consume(TokenType::RightParen, "expected ')' after a while condition");
  StmtPtr body = statement();
  return std::make_unique<WhileStmt>(std::move(cond), std::move(body), line);
}

StmtPtr Parser::for_statement() {
  const int line = previous().line;
  consume(TokenType::LeftParen, "expected '(' after 'for'");

  StmtPtr init;
  if (match(TokenType::Semicolon)) {
    // no initializer
  } else if (match(TokenType::Let)) {
    init = let_declaration();
  } else {
    init = expression_statement();
  }

  ExprPtr cond;
  if (!check(TokenType::Semicolon)) cond = expression();
  consume(TokenType::Semicolon, "expected ';' after a loop condition");

  ExprPtr increment;
  if (!check(TokenType::RightParen)) increment = expression();
  consume(TokenType::RightParen, "expected ')' after for clauses");

  StmtPtr body = statement();
  return std::make_unique<ForStmt>(std::move(init), std::move(cond),
                                   std::move(increment), std::move(body), line);
}

StmtPtr Parser::return_statement() {
  const int line = previous().line;
  ExprPtr value;
  if (!check(TokenType::Semicolon)) value = expression();
  consume(TokenType::Semicolon, "expected ';' after a return value");
  return std::make_unique<ReturnStmt>(std::move(value), line);
}

StmtPtr Parser::print_statement() {
  const int line = previous().line;
  ExprPtr value = expression();
  consume(TokenType::Semicolon, "expected ';' after a print value");
  return std::make_unique<PrintStmt>(std::move(value), line);
}

StmtPtr Parser::expression_statement() {
  const int line = peek().line;
  ExprPtr value = expression();
  consume(TokenType::Semicolon, "expected ';' after an expression");
  return std::make_unique<ExpressionStmt>(std::move(value), line);
}

ExprPtr Parser::expression() { return parse_precedence(PrecAssignment); }

ExprPtr Parser::parse_precedence(Precedence min) {
  ExprPtr left = prefix();
  if (!left) return nullptr;

  while (!at_end()) {
    const Precedence prec = precedence_of(peek().type);
    if (prec < min || prec == PrecNone) break;
    left = infix(std::move(left));
    if (!left) return nullptr;
  }
  return left;
}

ExprPtr Parser::prefix() {
  const Token& token = advance();
  const int line = token.line;

  switch (token.type) {
    case TokenType::Number: {
      // strtod rather than from_chars: Apple clang's libc++ still lacks the
      // floating-point overload, and the exponent forms the lexer accepts have
      // to parse identically here.
      const std::string text(token.text);
      return std::make_unique<LiteralExpr>(Literal::of(std::strtod(text.c_str(), nullptr)), line);
    }
    case TokenType::String:
      return std::make_unique<LiteralExpr>(Literal::of(token.literal), line);
    case TokenType::True:
      return std::make_unique<LiteralExpr>(Literal::of(true), line);
    case TokenType::False:
      return std::make_unique<LiteralExpr>(Literal::of(false), line);
    case TokenType::Nil:
      return std::make_unique<LiteralExpr>(Literal::nil(), line);
    case TokenType::Identifier:
      return std::make_unique<VariableExpr>(std::string(token.text), line);
    case TokenType::Bang:
    case TokenType::Minus: {
      ExprPtr operand = parse_precedence(PrecUnary);
      if (!operand) return nullptr;
      return std::make_unique<UnaryExpr>(token.type, std::move(operand), line);
    }
    case TokenType::LeftParen: {
      ExprPtr inner = expression();
      consume(TokenType::RightParen, "expected ')' after an expression");
      return inner;
    }
    case TokenType::LeftBracket: {
      std::vector<ExprPtr> items;
      if (!check(TokenType::RightBracket)) {
        do {
          if (check(TokenType::RightBracket)) break;  // allow a trailing comma
          items.push_back(expression());
        } while (match(TokenType::Comma));
      }
      consume(TokenType::RightBracket, "expected ']' after list items");
      return std::make_unique<ListExpr>(std::move(items), line);
    }
    case TokenType::Fn: {
      auto body = function_body("<lambda>");
      return std::make_unique<LambdaExpr>(std::move(body), line);
    }
    case TokenType::Error:
      error_at(token, token.literal);
      return nullptr;
    default:
      error_at(token, "expected an expression");
      return nullptr;
  }
}

ExprPtr Parser::infix(ExprPtr left) {
  const Token& token = advance();
  const int line = token.line;

  switch (token.type) {
    case TokenType::Equal: {
      // Assignment is right associative, so the right side is parsed at the
      // same precedence rather than one higher.
      ExprPtr value = parse_precedence(PrecAssignment);
      if (!value) return nullptr;
      if (left->kind == Expr::Kind::Variable) {
        auto* var = static_cast<VariableExpr*>(left.get());
        return std::make_unique<AssignExpr>(var->name, std::move(value), line);
      }
      if (left->kind == Expr::Kind::Index) {
        auto* idx = static_cast<IndexExpr*>(left.get());
        return std::make_unique<IndexAssignExpr>(
            std::move(idx->target), std::move(idx->index), std::move(value), line);
      }
      error_at(token, "invalid assignment target");
      return nullptr;
    }
    case TokenType::Or:
    case TokenType::And: {
      ExprPtr right = parse_precedence(
          static_cast<Precedence>(precedence_of(token.type) + 1));
      if (!right) return nullptr;
      return std::make_unique<LogicalExpr>(token.type, std::move(left),
                                           std::move(right), line);
    }
    case TokenType::LeftParen:
      return finish_call(std::move(left));
    case TokenType::LeftBracket: {
      ExprPtr index = expression();
      consume(TokenType::RightBracket, "expected ']' after an index");
      return std::make_unique<IndexExpr>(std::move(left), std::move(index), line);
    }
    default: {
      // Left associative: the right operand binds one level tighter, so
      // `a - b - c` groups as `(a - b) - c`.
      ExprPtr right = parse_precedence(
          static_cast<Precedence>(precedence_of(token.type) + 1));
      if (!right) return nullptr;
      return std::make_unique<BinaryExpr>(token.type, std::move(left),
                                          std::move(right), line);
    }
  }
}

ExprPtr Parser::finish_call(ExprPtr callee) {
  const int line = previous().line;
  std::vector<ExprPtr> args;
  if (!check(TokenType::RightParen)) {
    do {
      if (args.size() >= 255) {
        error_at(peek(), "a call cannot have more than 255 arguments");
        return nullptr;
      }
      args.push_back(expression());
    } while (match(TokenType::Comma));
  }
  consume(TokenType::RightParen, "expected ')' after arguments");
  return std::make_unique<CallExpr>(std::move(callee), std::move(args), line);
}

}  // namespace lumen
