#include "lumen/chunk.hpp"

#include <cstdio>
#include <sstream>

#include "lumen/object.hpp"

namespace lumen {

const char* op_name(Op op) {
  // Generated from the same list as the enum, so a new opcode cannot be added
  // without a name.
  static const char* const names[] = {
#define LUMEN_OP_NAME(name, text) text,
      LUMEN_OPCODES(LUMEN_OP_NAME)
#undef LUMEN_OP_NAME
  };
  static_assert(sizeof(names) / sizeof(names[0]) ==
                    static_cast<std::size_t>(Op::OpCount),
                "opcode name table is out of step with the enum");
  const auto index = static_cast<std::size_t>(op);
  if (index >= static_cast<std::size_t>(Op::OpCount)) return "?";
  return names[index];
}

void Chunk::write_byte(std::uint8_t byte, int line) {
  code_.push_back(byte);
  if (!lines_.empty() && lines_.back().line == line) {
    ++lines_.back().count;
  } else {
    lines_.push_back(LineRun{line, 1});
  }
}

void Chunk::write(Op op, int line) {
  write_byte(static_cast<std::uint8_t>(op), line);
}

void Chunk::write_u16(std::uint16_t value, int line) {
  write_byte(static_cast<std::uint8_t>(value >> 8), line);
  write_byte(static_cast<std::uint8_t>(value & 0xff), line);
}

int Chunk::add_constant(Value value) {
  for (std::size_t i = 0; i < constants_.size(); ++i) {
    // Compared by bit pattern, not by ==: two interned strings with the same
    // contents are the same pointer, and two numbers with the same bits are
    // interchangeable. Using == here would fold NaN constants together, since
    // NaN != NaN would report them as distinct forever and grow the pool.
    if (constants_[i].bits() == value.bits()) return static_cast<int>(i);
  }
  constants_.push_back(value);
  return static_cast<int>(constants_.size() - 1);
}

void Chunk::patch_u16(std::size_t offset, std::uint16_t value) {
  code_[offset] = static_cast<std::uint8_t>(value >> 8);
  code_[offset + 1] = static_cast<std::uint8_t>(value & 0xff);
}

int Chunk::line_at(std::size_t offset) const {
  std::size_t seen = 0;
  for (const auto& run : lines_) {
    seen += static_cast<std::size_t>(run.count);
    if (offset < seen) return run.line;
  }
  return lines_.empty() ? 0 : lines_.back().line;
}

std::vector<int> Chunk::expand_lines() const {
  std::vector<int> out;
  out.reserve(code_.size());
  for (const auto& run : lines_) {
    for (int i = 0; i < run.count; ++i) out.push_back(run.line);
  }
  out.resize(code_.size(), out.empty() ? 0 : out.back());
  return out;
}

void Chunk::set_lines(std::vector<int> per_byte_lines) {
  lines_.clear();
  for (int line : per_byte_lines) {
    if (!lines_.empty() && lines_.back().line == line) {
      ++lines_.back().count;
    } else {
      lines_.push_back(LineRun{line, 1});
    }
  }
}

namespace {


std::string pad(const std::string& s, std::size_t width) {
  std::string out = s;
  while (out.size() < width) out.push_back(' ');
  return out;
}

}  // namespace

std::size_t Chunk::disassemble_instruction(std::string& out, std::size_t offset) const {
  std::ostringstream os;
  os.width(4);
  os.fill('0');
  os << offset;
  os.fill(' ');
  os << "  ";

  if (offset > 0 && line_at(offset) == line_at(offset - 1)) {
    os << "   | ";
  } else {
    os.width(4);
    os << line_at(offset) << " ";
  }

  const Op op = static_cast<Op>(code_[offset]);
  os << pad(op_name(op), 20);

  auto constant_name = [&](std::uint16_t idx) {
    if (idx < constants_.size()) return constants_[idx].to_string();
    return std::string("<bad constant ") + std::to_string(idx) + ">";
  };

  switch (op) {
    case Op::Constant:
    case Op::GetGlobal:
    case Op::DefineGlobal:
    case Op::SetGlobal: {
      const std::uint16_t idx = u16_at(offset + 1);
      os << idx << " (" << constant_name(idx) << ")\n";
      out += os.str();
      return offset + 3;
    }
    case Op::GetLocal:
    case Op::SetLocal:
    case Op::GetUpvalue:
    case Op::SetUpvalue:
    case Op::Call:
    case Op::IncLocal: {
      os << static_cast<int>(code_[offset + 1]) << "\n";
      out += os.str();
      return offset + 2;
    }
    case Op::GetLocalGetLocal: {
      os << static_cast<int>(code_[offset + 1]) << " "
         << static_cast<int>(code_[offset + 2]) << "\n";
      out += os.str();
      return offset + 3;
    }
    case Op::AddLocalConst: {
      const std::uint16_t idx = u16_at(offset + 2);
      os << static_cast<int>(code_[offset + 1]) << " " << idx << " ("
         << constant_name(idx) << ")\n";
      out += os.str();
      return offset + 4;
    }
    case Op::Jump:
    case Op::JumpIfFalse:
    case Op::JumpIfTrue:
    case Op::PopJumpIfFalse: {
      const std::uint16_t jump = u16_at(offset + 1);
      os << jump << " -> " << (offset + 3 + jump) << "\n";
      out += os.str();
      return offset + 3;
    }
    case Op::Loop: {
      const std::uint16_t jump = u16_at(offset + 1);
      os << jump << " -> " << (offset + 3 - jump) << "\n";
      out += os.str();
      return offset + 3;
    }
    case Op::BuildList: {
      os << u16_at(offset + 1) << "\n";
      out += os.str();
      return offset + 3;
    }
    case Op::Closure: {
      const std::uint16_t idx = u16_at(offset + 1);
      os << idx << " (" << constant_name(idx) << ")\n";
      std::size_t next = offset + 3;
      int upvalues = 0;
      if (idx < constants_.size() && constants_[idx].is_object()) {
        const Obj* o = constants_[idx].as_object();
        if (o->type == ObjType::Function) {
          upvalues = static_cast<const ObjFunction*>(o)->upvalue_count;
        }
      }
      for (int i = 0; i < upvalues; ++i) {
        std::ostringstream sub;
        sub.width(4);
        sub.fill('0');
        sub << next;
        sub.fill(' ');
        sub << "  " << "   |   " << (code_[next] ? "local " : "upvalue ")
            << static_cast<int>(code_[next + 1]) << "\n";
        os << sub.str();
        next += 2;
      }
      out += os.str();
      return next;
    }
    default:
      os << "\n";
      out += os.str();
      return offset + 1;
  }
}

std::string Chunk::disassemble(const std::string& name) const {
  std::string out = "== " + name + " ==\n";
  std::size_t offset = 0;
  while (offset < code_.size()) {
    offset = disassemble_instruction(out, offset);
  }
  return out;
}

}  // namespace lumen
