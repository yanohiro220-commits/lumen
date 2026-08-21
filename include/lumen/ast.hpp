#pragma once

#include <memory>
#include <string>
#include <vector>

#include "lumen/lexer.hpp"

namespace lumen {

// A compile-time literal. Deliberately not a runtime Value: the garbage
// collector is not running during parsing, so a string literal here is a plain
// std::string and only becomes a heap object at code generation.
struct Literal {
  enum class Type { Nil, Bool, Number, String };
  Type type = Type::Nil;
  bool boolean = false;
  double number = 0.0;
  std::string string;

  static Literal nil() { return Literal{}; }
  static Literal of(bool b) {
    Literal l;
    l.type = Type::Bool;
    l.boolean = b;
    return l;
  }
  static Literal of(double d) {
    Literal l;
    l.type = Type::Number;
    l.number = d;
    return l;
  }
  static Literal of(std::string s) {
    Literal l;
    l.type = Type::String;
    l.string = std::move(s);
    return l;
  }

  bool truthy() const {
    return !(type == Type::Nil || (type == Type::Bool && !boolean));
  }
  std::string to_string() const;
};

struct Expr;
struct Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

// The AST is a tagged hierarchy rather than a visitor interface.
//
// Three passes walk it - the optimizer, the resolver and the code generator -
// and two of them rewrite nodes in place. A visitor that has to return a
// replacement node for every case is more ceremony than a switch on a kind
// tag, and the tag makes the optimizer's pattern matching direct.
struct Expr {
  enum class Kind {
    Literal, Variable, Assign, Unary, Binary, Logical, Call,
    ListLiteral, Index, IndexAssign, Lambda,
  };

  Kind kind;
  int line = 0;
  explicit Expr(Kind k, int ln) : kind(k), line(ln) {}
  virtual ~Expr() = default;
};

struct LiteralExpr : Expr {
  Literal value;
  LiteralExpr(Literal v, int ln)
      : Expr(Kind::Literal, ln), value(std::move(v)) {}
};

struct VariableExpr : Expr {
  std::string name;
  // Filled by the resolver. slot < 0 means the name is a global, resolved by
  // hash at run time; otherwise it is a stack slot or an upvalue index.
  int slot = -1;
  bool is_upvalue = false;
  VariableExpr(std::string n, int ln)
      : Expr(Kind::Variable, ln), name(std::move(n)) {}
};

struct AssignExpr : Expr {
  std::string name;
  ExprPtr value;
  int slot = -1;
  bool is_upvalue = false;
  AssignExpr(std::string n, ExprPtr v, int ln)
      : Expr(Kind::Assign, ln), name(std::move(n)), value(std::move(v)) {}
};

struct UnaryExpr : Expr {
  TokenType op;
  ExprPtr operand;
  UnaryExpr(TokenType o, ExprPtr e, int ln)
      : Expr(Kind::Unary, ln), op(o), operand(std::move(e)) {}
};

struct BinaryExpr : Expr {
  TokenType op;
  ExprPtr left, right;
  BinaryExpr(TokenType o, ExprPtr l, ExprPtr r, int ln)
      : Expr(Kind::Binary, ln), op(o), left(std::move(l)), right(std::move(r)) {}
};

// `and` / `or`, kept separate from Binary because they short circuit and so
// compile to jumps rather than to an operator.
struct LogicalExpr : Expr {
  TokenType op;
  ExprPtr left, right;
  LogicalExpr(TokenType o, ExprPtr l, ExprPtr r, int ln)
      : Expr(Kind::Logical, ln), op(o), left(std::move(l)), right(std::move(r)) {}
};

struct CallExpr : Expr {
  ExprPtr callee;
  std::vector<ExprPtr> args;
  CallExpr(ExprPtr c, std::vector<ExprPtr> a, int ln)
      : Expr(Kind::Call, ln), callee(std::move(c)), args(std::move(a)) {}
};

struct ListExpr : Expr {
  std::vector<ExprPtr> items;
  ListExpr(std::vector<ExprPtr> i, int ln)
      : Expr(Kind::ListLiteral, ln), items(std::move(i)) {}
};

struct IndexExpr : Expr {
  ExprPtr target, index;
  IndexExpr(ExprPtr t, ExprPtr i, int ln)
      : Expr(Kind::Index, ln), target(std::move(t)), index(std::move(i)) {}
};

struct IndexAssignExpr : Expr {
  ExprPtr target, index, value;
  IndexAssignExpr(ExprPtr t, ExprPtr i, ExprPtr v, int ln)
      : Expr(Kind::IndexAssign, ln), target(std::move(t)), index(std::move(i)),
        value(std::move(v)) {}
};

struct FunctionBody;

struct LambdaExpr : Expr {
  std::shared_ptr<FunctionBody> body;
  LambdaExpr(std::shared_ptr<FunctionBody> b, int ln)
      : Expr(Kind::Lambda, ln), body(std::move(b)) {}
};

struct Stmt {
  enum class Kind {
    Expression, Print, Let, Block, If, While, For, Return, Function,
    Break, Continue,
  };
  Kind kind;
  int line = 0;
  explicit Stmt(Kind k, int ln) : kind(k), line(ln) {}
  virtual ~Stmt() = default;
};

struct ExpressionStmt : Stmt {
  ExprPtr expr;
  ExpressionStmt(ExprPtr e, int ln)
      : Stmt(Kind::Expression, ln), expr(std::move(e)) {}
};

struct PrintStmt : Stmt {
  ExprPtr expr;
  PrintStmt(ExprPtr e, int ln) : Stmt(Kind::Print, ln), expr(std::move(e)) {}
};

struct LetStmt : Stmt {
  std::string name;
  ExprPtr initializer;  // may be null, meaning nil
  int slot = -1;
  LetStmt(std::string n, ExprPtr init, int ln)
      : Stmt(Kind::Let, ln), name(std::move(n)), initializer(std::move(init)) {}
};

struct BlockStmt : Stmt {
  std::vector<StmtPtr> statements;
  BlockStmt(std::vector<StmtPtr> s, int ln)
      : Stmt(Kind::Block, ln), statements(std::move(s)) {}
};

struct IfStmt : Stmt {
  ExprPtr condition;
  StmtPtr then_branch;
  StmtPtr else_branch;  // may be null
  IfStmt(ExprPtr c, StmtPtr t, StmtPtr e, int ln)
      : Stmt(Kind::If, ln), condition(std::move(c)), then_branch(std::move(t)),
        else_branch(std::move(e)) {}
};

struct WhileStmt : Stmt {
  ExprPtr condition;
  StmtPtr body;
  WhileStmt(ExprPtr c, StmtPtr b, int ln)
      : Stmt(Kind::While, ln), condition(std::move(c)), body(std::move(b)) {}
};

// `for` keeps its own node rather than desugaring to a while loop, because
// `continue` has to jump to the increment clause and not to the condition. A
// desugared form loses that distinction and silently makes `continue` skip the
// increment, which is an infinite loop.
struct ForStmt : Stmt {
  StmtPtr initializer;  // may be null
  ExprPtr condition;    // may be null, meaning always true
  ExprPtr increment;    // may be null
  StmtPtr body;
  ForStmt(StmtPtr i, ExprPtr c, ExprPtr inc, StmtPtr b, int ln)
      : Stmt(Kind::For, ln), initializer(std::move(i)), condition(std::move(c)),
        increment(std::move(inc)), body(std::move(b)) {}
};

struct ReturnStmt : Stmt {
  ExprPtr value;  // may be null
  ReturnStmt(ExprPtr v, int ln)
      : Stmt(Kind::Return, ln), value(std::move(v)) {}
};

struct FunctionBody {
  std::string name;
  std::vector<std::string> params;
  std::vector<StmtPtr> body;
  int line = 0;
};

struct FunctionStmt : Stmt {
  std::shared_ptr<FunctionBody> body;
  int slot = -1;
  FunctionStmt(std::shared_ptr<FunctionBody> b, int ln)
      : Stmt(Kind::Function, ln), body(std::move(b)) {}
};

struct BreakStmt : Stmt {
  explicit BreakStmt(int ln) : Stmt(Kind::Break, ln) {}
};

struct ContinueStmt : Stmt {
  explicit ContinueStmt(int ln) : Stmt(Kind::Continue, ln) {}
};

// A parsed program: top-level statements.
struct Program {
  std::vector<StmtPtr> statements;
};

}  // namespace lumen
