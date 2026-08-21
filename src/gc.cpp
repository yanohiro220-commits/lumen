#include "lumen/gc.hpp"

namespace lumen {
namespace {

// Approximate heap cost of an object, used only to decide when to collect.
// Exactness does not matter; proportionality does.
std::size_t size_of(const Obj* obj) {
  switch (obj->type) {
    case ObjType::String:
      return sizeof(ObjString) + static_cast<const ObjString*>(obj)->chars.capacity();
    case ObjType::Function: {
      const auto* fn = static_cast<const ObjFunction*>(obj);
      return sizeof(ObjFunction) + fn->chunk.code().size() +
             fn->chunk.constants().size() * sizeof(Value);
    }
    case ObjType::Closure: {
      const auto* c = static_cast<const ObjClosure*>(obj);
      return sizeof(ObjClosure) + c->upvalues.capacity() * sizeof(ObjUpvalue*);
    }
    case ObjType::Upvalue:
      return sizeof(ObjUpvalue);
    case ObjType::Native:
      return sizeof(ObjNative);
    case ObjType::List: {
      const auto* l = static_cast<const ObjList*>(obj);
      return sizeof(ObjList) + l->items.capacity() * sizeof(Value);
    }
  }
  return sizeof(Obj);
}

}  // namespace

GC::~GC() {
  Obj* obj = objects_;
  while (obj) {
    Obj* next = obj->next;
    delete obj;
    obj = next;
  }
  objects_ = nullptr;
}

void GC::track(Obj* obj, std::size_t size) {
  obj->next = objects_;
  objects_ = obj;
  stats_.bytes_allocated += size;
  ++stats_.objects_live;
}

void GC::maybe_collect() {
  if (collecting_) return;
  if (stats_.next_gc == 0) stats_.next_gc = kInitialThreshold;
  if (stress_ || stats_.bytes_allocated > stats_.next_gc) collect();
}

ObjString* GC::intern(const std::string& text) {
  const auto it = strings_.find(text);
  if (it != strings_.end()) return it->second;

  maybe_collect();
  auto* obj = new ObjString(text);
  track(obj, size_of(obj));
  strings_.emplace(text, obj);
  ++stats_.strings_interned;
  return obj;
}

ObjString* GC::new_string(std::string text) {
  maybe_collect();
  auto* obj = new ObjString(std::move(text));
  track(obj, size_of(obj));
  return obj;
}

ObjString* GC::string(std::string text) {
  if (text.size() <= kInternThreshold) return intern(text);
  return new_string(std::move(text));
}

ObjFunction* GC::new_function() {
  maybe_collect();
  auto* fn = new ObjFunction();
  track(fn, sizeof(ObjFunction));
  return fn;
}

ObjClosure* GC::new_closure(ObjFunction* function) {
  maybe_collect();
  auto* c = new ObjClosure(function);
  track(c, size_of(c));
  return c;
}

ObjUpvalue* GC::new_upvalue(Value* slot) {
  maybe_collect();
  auto* u = new ObjUpvalue(slot);
  track(u, sizeof(ObjUpvalue));
  return u;
}

ObjNative* GC::new_native(NativeFn fn, ObjString* name, int arity) {
  maybe_collect();
  auto* n = new ObjNative(std::move(fn), name, arity);
  track(n, sizeof(ObjNative));
  return n;
}

ObjList* GC::new_list() {
  maybe_collect();
  auto* l = new ObjList();
  track(l, sizeof(ObjList));
  return l;
}

void GC::pop_temp(std::size_t n) {
  while (n-- > 0 && !temp_roots_.empty()) temp_roots_.pop_back();
}

void GC::mark_value(const Value& v) {
  if (v.is_object()) mark_object(v.as_object());
}

void GC::mark_object(Obj* obj) {
  if (obj == nullptr || obj->marked) return;
  obj->marked = true;
  // Gray set as an explicit worklist rather than recursion: a long list or a
  // deep closure chain would otherwise overflow the C++ stack during a
  // collection, which is an unrecoverable crash in the middle of the heap
  // being inconsistent.
  gray_.push_back(obj);
}

void GC::blacken(Obj* obj) {
  switch (obj->type) {
    case ObjType::String:
      break;
    case ObjType::Upvalue:
      mark_value(static_cast<ObjUpvalue*>(obj)->closed);
      break;
    case ObjType::Function: {
      auto* fn = static_cast<ObjFunction*>(obj);
      mark_object(fn->name);
      for (const Value& v : fn->chunk.constants()) mark_value(v);
      break;
    }
    case ObjType::Closure: {
      auto* c = static_cast<ObjClosure*>(obj);
      mark_object(c->function);
      for (ObjUpvalue* u : c->upvalues) mark_object(u);
      break;
    }
    case ObjType::Native:
      mark_object(static_cast<ObjNative*>(obj)->name);
      break;
    case ObjType::List:
      for (const Value& v : static_cast<ObjList*>(obj)->items) mark_value(v);
      break;
  }
}

void GC::trace_references() {
  while (!gray_.empty()) {
    Obj* obj = gray_.back();
    gray_.pop_back();
    blacken(obj);
  }
}

void GC::remove_unmarked_strings() {
  // The intern table is weak. Entries have to be dropped before the sweep,
  // while the mark bits still say what is live; afterwards the pointers dangle.
  for (auto it = strings_.begin(); it != strings_.end();) {
    if (!it->second->marked) {
      it = strings_.erase(it);
    } else {
      ++it;
    }
  }
}

void GC::sweep() {
  Obj* previous = nullptr;
  Obj* obj = objects_;
  while (obj) {
    if (obj->marked) {
      obj->marked = false;
      previous = obj;
      obj = obj->next;
      continue;
    }
    Obj* unreached = obj;
    obj = obj->next;
    if (previous) {
      previous->next = obj;
    } else {
      objects_ = obj;
    }
    stats_.bytes_allocated -= size_of(unreached);
    --stats_.objects_live;
    ++stats_.objects_freed;
    delete unreached;
  }
}

void GC::collect() {
  if (collecting_) return;
  collecting_ = true;

  for (Obj* obj : temp_roots_) mark_object(obj);
  if (root_marker_) root_marker_(*this);
  trace_references();

  remove_unmarked_strings();
  sweep();

  stats_.next_gc = stats_.bytes_allocated * kHeapGrowFactor;
  if (stats_.next_gc < kInitialThreshold) stats_.next_gc = kInitialThreshold;
  ++stats_.collections;
  collecting_ = false;
}

std::size_t GC::live_objects() const { return stats_.objects_live; }

}  // namespace lumen
