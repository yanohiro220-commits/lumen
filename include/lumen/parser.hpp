#pragma once

#include <string>
#include <vector>

#include "lumen/ast.hpp"
#include "lumen/lexer.hpp"

namespace lumen {

struct Diagnostic {
  int line = 0;
  int column = 0;
  std::string message;

  std::string format() const;
};

// Recursive descent for statements, Pratt parsing for expressions.
//
// Pratt parsing is the right shape for expressions because precedence lives in
// a table rather than in the call graph: adding an operator is one table entry,
// not a new grammar rule threaded between two existing ones. The classic
// alternative - one function per precedence level - makes every addition a
// refactor of the whole chain.
class Parser {
 public:
  explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

  // Parses the whole token stream. On error the returned program is
  // incomplete; check errors(). Parsing continues past a bad statement so one
  // run reports several problems rather than one at a time.
  Program parse();

  const std::vector<Diagnostic>& errors() const { return errors_; }
  bool had_error() const { return !errors_.empty(); }

 private:
  // Binding powers. Higher binds tighter; the table is the whole precedence
  // specification for the language.
  enum Precedence : int {
    PrecNone = 0,
    PrecAssignment,  // =
    PrecOr,          // or
    PrecAnd,         // and
    PrecEquality,    // == !=
    PrecComparison,  // < > <= >=
    PrecTerm,        // + -
    PrecFactor,      // * / %
    PrecUnary,       // ! -
    PrecCall,        // () []
    PrecPrimary,
  };

  static Precedence precedence_of(TokenType t);

  const Token& peek() const { return tokens_[pos_]; }
  const Token& previous() const { return tokens_[pos_ - 1]; }
  bool at_end() const { return peek().type == TokenType::Eof; }
  const Token& advance();
  bool check(TokenType t) const { return peek().type == t; }
  bool match(TokenType t);
  const Token& consume(TokenType t, const std::string& message);
  void error_at(const Token& token, const std::string& message);
  // Discards tokens until the start of what is plausibly a new statement, so
  // one syntax error does not cascade into a page of noise.
  void synchronize();

  StmtPtr declaration();
  StmtPtr let_declaration();
  StmtPtr function_declaration();
  StmtPtr statement();
  StmtPtr block_statement();
  StmtPtr if_statement();
  StmtPtr while_statement();
  StmtPtr for_statement();
  StmtPtr return_statement();
  StmtPtr print_statement();
  StmtPtr expression_statement();

  ExprPtr expression();
  ExprPtr parse_precedence(Precedence min);
  ExprPtr prefix();
  ExprPtr infix(ExprPtr left);
  ExprPtr finish_call(ExprPtr callee);
  std::shared_ptr<FunctionBody> function_body(const std::string& name);

  std::vector<Token> tokens_;
  std::size_t pos_ = 0;
  std::vector<Diagnostic> errors_;
  bool panic_ = false;
};

}  // namespace lumen
