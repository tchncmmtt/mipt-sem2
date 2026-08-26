#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
using std::byte;
using std::invoke;

namespace objects {
union Storage {
    void* ptr;
    alignas(std::max_align_t) byte storage[16];
};

template <typename T>
constexpr bool kIsLocal = sizeof(T) <= 16;

}  // namespace objects

struct VirTable {
    void (*destroy)(objects::Storage&) noexcept;
    void (*copy)(objects::Storage&, const objects::Storage&);
    void (*move)(objects::Storage&, objects::Storage&) noexcept;
    void* (*get)(const objects::Storage&) noexcept;
    const std::type_info& (*type)() noexcept;
};

template <typename T>
struct VirTableF {
    static void DesIml(objects::Storage& s) noexcept {
        if constexpr (objects::kIsLocal<T>) {
            reinterpret_cast<T*>(const_cast<byte*>(s.storage))->~T();
        } else {
            delete static_cast<T*>(s.ptr);
        }
    }

    static void CopyIml(objects::Storage& s1, const objects::Storage& s2) {
        if constexpr (std::is_copy_constructible_v<T>) {
            if constexpr (objects::kIsLocal<T>) {
                new (s1.storage) T(*reinterpret_cast<const T*>(s2.storage));
            } else {
                s1.ptr = new T(*static_cast<const T*>(s2.ptr));
            }
        }
    }

    static void MoveIml(objects::Storage& s1, objects::Storage& s2) noexcept {
        if constexpr (objects::kIsLocal<T>) {
            T* obj = reinterpret_cast<T*>(s2.storage);
            new (s1.storage) T(std::move(*obj));
            obj->~T();
        } else {
            s1.ptr = s2.ptr;
            s2.ptr = nullptr;
        }
    }

    static void* TargetIml(const objects::Storage& s) noexcept {
        if constexpr (objects::kIsLocal<T>) {
            const auto address = reinterpret_cast<uintptr_t>(s.storage);
            return reinterpret_cast<void*>(address);
        } else {
            return s.ptr;
        }
    }

    static const std::type_info& TypeIml() noexcept {
        return typeid(T);
    }

    static constexpr VirTable kTable = {.destroy = &DesIml, .copy = &CopyIml, .move = &MoveIml, .get = &TargetIml, .type = &TypeIml};
};

template <typename Signature, bool Copyable>
class FunctionBase;

template <typename ReturnType, typename... Args, bool Copyable>
class FunctionBase<ReturnType(Args...), Copyable> {
protected:
    objects::Storage storage_{};

    using InvPtr = ReturnType (*)(const objects::Storage&, Args...);
    InvPtr inv_ptr_ = nullptr;

    const VirTable* vir_table_ = nullptr;

    template <typename T>
    static ReturnType ImvIml(const objects::Storage& s, Args... args) {
        if constexpr (objects::kIsLocal<T>) {
            auto address = reinterpret_cast<uintptr_t>(s.storage);
            T& func = *reinterpret_cast<T*>(address);
            return invoke(func, std::forward<Args>(args)...);
        } else {
            return invoke(*static_cast<T*>(s.ptr), std::forward<Args>(args)...);
        }
    }

public:
    FunctionBase() = default;

    FunctionBase(std::nullptr_t) : inv_ptr_(nullptr) {
    }

    FunctionBase& operator=(std::nullptr_t) noexcept {
        if (vir_table_ != nullptr) {
            vir_table_->destroy(storage_);
            vir_table_ = nullptr;
        }
        inv_ptr_ = nullptr;
        return *this;
    }

    FunctionBase(FunctionBase&& f) noexcept {
        vir_table_ = f.vir_table_;
        inv_ptr_ = f.inv_ptr_;
        if (f.vir_table_ != nullptr) {
            f.vir_table_->move(storage_, f.storage_);
            f.vir_table_ = nullptr;
            f.inv_ptr_ = nullptr;
        }
    }

    FunctionBase& operator=(FunctionBase&& f) noexcept {
        if (this != &f) {
            if (vir_table_ != nullptr) {
                vir_table_->destroy(storage_);
            }
            inv_ptr_ = f.inv_ptr_;
            vir_table_ = f.vir_table_;
            if (f.vir_table_ != nullptr) {
                f.vir_table_->move(storage_, f.storage_);
                f.vir_table_ = nullptr;
                f.inv_ptr_ = nullptr;
            }
        }
        return *this;
    }

    FunctionBase(const FunctionBase& f)
        requires Copyable
    {
        if (f.vir_table_ != nullptr) {
            f.vir_table_->copy(storage_, f.storage_);
            vir_table_ = f.vir_table_;
            inv_ptr_ = f.inv_ptr_;
        }
    }

    FunctionBase& operator=(const FunctionBase& f)
        requires Copyable
    {
        if (this != &f) {
            FunctionBase temp(f);
            *this = std::move(temp);
        }
        return *this;
    }

    template <typename F, typename Decayed = std::decay_t<F>>
        requires(!std::is_base_of_v<FunctionBase, Decayed> &&
                 (!Copyable || std::is_copy_constructible_v<Decayed>) &&
                 std::is_invocable_r_v<ReturnType, Decayed&, Args...>)
    FunctionBase(F&& f) {
        if constexpr (objects::kIsLocal<Decayed>) {
            new (storage_.storage) Decayed(std::forward<F>(f));
        } else {
            storage_.ptr = new Decayed(std::forward<F>(f));
        }
        inv_ptr_ = &ImvIml<Decayed>;
        vir_table_ = &VirTableF<Decayed>::kTable;
    }

    template <typename F, typename Decayed = std::decay_t<F>>
        requires(!std::is_base_of_v<FunctionBase, Decayed> &&
                 (!Copyable || std::is_copy_constructible_v<Decayed>) &&
                 std::is_invocable_r_v<ReturnType, Decayed&, Args...>)
    FunctionBase& operator=(F&& f) {
        FunctionBase temp(std::forward<F>(f));
        *this = std::move(temp);
        return *this;
    }

    ~FunctionBase() noexcept {
        if (vir_table_ != nullptr) {
            vir_table_->destroy(storage_);
        }
    }

    ReturnType operator()(Args... args) const {
        if (inv_ptr_ == nullptr) {
            throw std::bad_function_call();
        }
        return inv_ptr_(storage_, std::forward<Args>(args)...);
    }

    explicit operator bool() const noexcept {
        return inv_ptr_ != nullptr;
    }

    [[nodiscard]] const std::type_info& TargetType() const noexcept {
        return vir_table_ ? vir_table_->type() : typeid(void);
    }

    template <typename T>
    T* Target() noexcept {
        if (vir_table_ && TargetType() == typeid(T)) {
            return static_cast<T*>(vir_table_->get(storage_));
        }
        return nullptr;
    }

    template <typename T>
    const T* Target() const noexcept {
        if (vir_table_ && TargetType() == typeid(T)) {
            return static_cast<const T*>(vir_table_->get(storage_));
        }
        return nullptr;
    }

    friend bool operator==(const FunctionBase& f, std::nullptr_t) noexcept {
        return !f;
    }

    friend bool operator==(std::nullptr_t, const FunctionBase& f) noexcept {
        return !f;
    }
};

template <typename Signature>
class Function;

template <typename ReturnType, typename... Args>
class Function<ReturnType(Args...)> : public FunctionBase<ReturnType(Args...), true> {
    using Base = FunctionBase<ReturnType(Args...), true>;

public:
    using Base::Base;
    using Base::operator=;
};

template <typename Signature>
class MoveOnlyFunction;

template <typename ReturnType, typename... Args>
class MoveOnlyFunction<ReturnType(Args...)> : public FunctionBase<ReturnType(Args...), false> {
    using Base = FunctionBase<ReturnType(Args...), false>;

public:
    using Base::Base;
    using Base::operator=;
};

namespace sigs {
template <typename T>
struct Sig;

template <typename R, typename C, typename... Args>
struct Sig<R (C::*)(Args...) const> {
    using Type = R(Args...);
};

template <typename R, typename C, typename... Args>
struct Sig<R (C::*)(Args...)> {
    using Type = R(Args...);
};
}  // namespace sigs

template <typename R, typename... Args>
Function(R (*)(Args...)) -> Function<R(Args...)>;
template <typename R, typename... Args>
MoveOnlyFunction(R (*)(Args...)) -> MoveOnlyFunction<R(Args...)>;

template <typename F>
Function(F) -> Function<typename sigs::Sig<decltype(&std::decay_t<F>::operator())>::Type>;
template <typename F>
MoveOnlyFunction(F)
    -> MoveOnlyFunction<typename sigs::Sig<decltype(&std::decay_t<F>::operator())>::Type>;