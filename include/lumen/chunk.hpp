#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "lumen/value.hpp"

namespace lumen {

// Instruction set.
//
// A stack machine rather than a register machine: the code generator falls
// straight out of a post-order AST walk with no register allocator, and the
// dispatch loop stays small enough to stay in cache. The cost is more
// instructions executed per unit of work, which the superinstructions at the
// end claw back for the cases that matter.
//
// The opcodes are declared once, in an X-macro, and the enum, the name table
// and the VM's computed-goto dispatch table are all generated from it. Those
// three lists have to agree exactly - a dispatch table one entry out of step
// with the enum sends every instruction to the wrong handler - and generating
// them makes disagreement impossible rather than merely unlikely.
//
// Operand widths are in the comments: u8 for slots and counts, u16 for
// constant pool indices and jump distances.
// Each entry carries the enum name and the name used in a disassembly
// listing. Deriving the second from the first is not possible in a macro, and
// the listing convention is upper snake case, so both are written out.
#define LUMEN_OPCODES(X)                                                        \
  X(Constant, "CONSTANT")           /* u16 constant index */                    \
  X(Nil, "NIL") X(True, "TRUE") X(False, "FALSE") X(Pop, "POP")                 \
  X(GetLocal, "GET_LOCAL")          /* u8 stack slot */                         \
  X(SetLocal, "SET_LOCAL")          /* u8 stack slot */                         \
  X(GetGlobal, "GET_GLOBAL")        /* u16 constant naming the global */        \
  X(DefineGlobal, "DEFINE_GLOBAL")  /* u16 */                                   \
  X(SetGlobal, "SET_GLOBAL")        /* u16 */                                   \
  X(GetUpvalue, "GET_UPVALUE")      /* u8 upvalue index */                      \
  X(SetUpvalue, "SET_UPVALUE")      /* u8 */                                    \
  X(Equal, "EQUAL") X(NotEqual, "NOT_EQUAL")                                    \
  X(Greater, "GREATER") X(GreaterEqual, "GREATER_EQUAL")                        \
  X(Less, "LESS") X(LessEqual, "LESS_EQUAL")                                    \
  X(Add, "ADD") X(Subtract, "SUBTRACT") X(Multiply, "MULTIPLY")                 \
  X(Divide, "DIVIDE") X(Modulo, "MODULO")                                       \
  X(Negate, "NEGATE") X(Not, "NOT")                                             \
  X(Print, "PRINT")                                                             \
  X(Jump, "JUMP")                     /* u16 forward */                         \
  X(JumpIfFalse, "JUMP_IF_FALSE")     /* u16, peeks the condition */            \
  X(JumpIfTrue, "JUMP_IF_TRUE")       /* u16, peeks */                          \
  X(PopJumpIfFalse, "POP_JUMP_IF_FALSE") /* u16, pops the condition */          \
  X(Loop, "LOOP")                     /* u16 backward */                        \
  X(Call, "CALL")                     /* u8 argument count */                   \
  X(Closure, "CLOSURE")  /* u16 fn, then (isLocal u8, index u8) per upvalue */  \
  X(CloseUpvalue, "CLOSE_UPVALUE") X(Return, "RETURN")                          \
  X(BuildList, "BUILD_LIST")          /* u16 item count */                      \
  X(IndexGet, "INDEX_GET") X(IndexSet, "INDEX_SET")                             \
  /* Superinstructions, produced only by the peephole pass. Each folds a        \
     sequence the generator emits constantly, saving a dispatch and a stack     \
     round trip. */                                                             \
  X(AddLocalConst, "ADD_LOCAL_CONST") /* u8 slot, u16 constant */               \
  X(IncLocal, "INC_LOCAL")            /* u8 slot: local += 1 */                 \
  X(GetLocalGetLocal, "GET_LOCAL_GET_LOCAL") /* u8, u8 */

enum class Op : std::uint8_t {
#define LUMEN_OP_ENUM(name, text) name,
  LUMEN_OPCODES(LUMEN_OP_ENUM)
#undef LUMEN_OP_ENUM
      OpCount
};

const char* op_name(Op op);

// A compiled unit of code: instructions, a constant pool and line numbers.
//
// Line numbers are stored run-length encoded. Storing one int per byte of
// bytecode doubles the size of a chunk for information that is only ever read
// when producing a stack trace, and real code has long runs from the same line.
class Chunk {
 public:
  void write(Op op, int line);
  void write_byte(std::uint8_t byte, int line);
  void write_u16(std::uint16_t value, int line);

  // Adds a constant, reusing an existing entry when an identical one is
  // already present. Deduplication matters more than it looks: every global
  // access names its variable with a constant, so a loop body referencing the
  // same global ten times would otherwise pay for ten copies.
  int add_constant(Value value);

  std::size_t size() const { return code_.size(); }
  const std::vector<std::uint8_t>& code() const { return code_; }
  std::vector<std::uint8_t>& code() { return code_; }
  const std::vector<Value>& constants() const { return constants_; }
  std::vector<Value>& constants() { return constants_; }

  std::uint8_t at(std::size_t offset) const { return code_[offset]; }
  std::uint16_t u16_at(std::size_t offset) const {
    return static_cast<std::uint16_t>((code_[offset] << 8) | code_[offset + 1]);
  }
  void patch_u16(std::size_t offset, std::uint16_t value);

  int line_at(std::size_t offset) const;

  // Human-readable listing. Used by the tests to assert on generated code,
  // which is the only way to check an optimizer without running it.
  std::string disassemble(const std::string& name) const;
  std::size_t disassemble_instruction(std::string& out, std::size_t offset) const;

  // Rebuilds the line table after the peephole pass has rewritten `code_`.
  void set_lines(std::vector<int> per_byte_lines);
  std::vector<int> expand_lines() const;

 private:
  struct LineRun {
    int line;
    int count;
  };

  std::vector<std::uint8_t> code_;
  std::vector<Value> constants_;
  std::vector<LineRun> lines_;
};

}  // namespace lumen
