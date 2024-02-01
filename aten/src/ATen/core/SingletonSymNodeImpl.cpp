#include <ATen/core/SingletonSymNodeImpl.h>
#include <c10/core/SymNodeImpl.h>
#include <c10/util/Exception.h>

namespace c10 {

namespace {
bool _eq(const char* op, c10::SymNodeImpl* lhs, c10::SymNodeImpl* rhs) {
  TORCH_INTERNAL_ASSERT(lhs->singleton_int().has_value());
  c10::optional<int64_t> c = rhs->singleton_int();
  return (
      c.has_value() && lhs->singleton_int() == *c &&
      lhs->singleton_coeff() == rhs->singleton_coeff());
}
bool _ge(const char* op, c10::SymNodeImpl* lhs, c10::SymNodeImpl* rhs) {
  if (auto mb_si = lhs->singleton_int()) {
    if (auto mb_si2 = rhs->singleton_int()) {
      if (*mb_si == *mb_si2) {
        return lhs->singleton_coeff() >= rhs->singleton_coeff();
      }
      TORCH_CHECK(false, "Singleton int ", op, ": Relation is indeterminate");
    }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    if (rhs->constant_int() && *rhs->constant_int() <= 2) {
      return true;
    }
    TORCH_CHECK(false, "Singleton int ", op, ": Relation is indeterminate");
  } else if (rhs->singleton_int()) {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    if (lhs->constant_int() && *lhs->constant_int() < 2) {
      return false;
    }
    TORCH_CHECK(false, "Singleton int ", op, ": Relation is indeterminate");
  }
  TORCH_INTERNAL_ASSERT(false, "expect at least one singleton");
}
} // namespace

c10::SymNode SingletonSymNodeImpl::eq(const c10::SymNode& other) {
  return SymNode(c10::make_intrusive<ConstantSymNodeImpl<bool>>(
      _eq("eq", this, other.get())));
}

c10::SymNode SingletonSymNodeImpl::ne(const c10::SymNode& other) {
  return SymNode(c10::make_intrusive<ConstantSymNodeImpl<bool>>(
      !_eq("ne", this, other.get())));
}

c10::SymNode SingletonSymNodeImpl::ge(const c10::SymNode& other) {
  return SymNode(c10::make_intrusive<ConstantSymNodeImpl<bool>>(
      _ge("ge", this, other.get())));
}

c10::SymNode SingletonSymNodeImpl::gt(const c10::SymNode& other) {
  return SymNode(c10::make_intrusive<ConstantSymNodeImpl<bool>>(
      !_ge("gt", other.get(), this)));
}

c10::SymNode SingletonSymNodeImpl::lt(const c10::SymNode& other) {
  return SymNode(c10::make_intrusive<ConstantSymNodeImpl<bool>>(
      !_ge("lt", this, other.get())));
}

c10::SymNode SingletonSymNodeImpl::le(const c10::SymNode& other) {
  return SymNode(c10::make_intrusive<ConstantSymNodeImpl<bool>>(
      _ge("le", other.get(), this)));
}

c10::SymNode SingletonSymNodeImpl::mul(const c10::SymNode& other) {
  if (auto mb_si = other->singleton_int()) {
    TORCH_CHECK(false, "Singleton int cannot be multiplied by singleton int");
  }
  c10::optional<int64_t> c = other->constant_int();
  TORCH_CHECK(c.has_value());
  return SymNode(c10::make_intrusive<SingletonSymNodeImpl>(val_, coeff_ * *c));
}

} // namespace c10
