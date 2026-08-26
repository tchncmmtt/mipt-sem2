#pragma once

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

using std::allocator;
using std::allocator_traits;
using std::enable_if_t;
using std::is_convertible_v;
using std::swap;

template <typename T>
class EnableSharedFromThis;
template <typename T>
class WeakPtr;

struct ControlBlock {
    size_t shared_count = 1;
    size_t weak_count = 1;

    virtual void Dispose() = 0;
    virtual void Destroy() = 0;
    virtual ~ControlBlock() = default;
};

template <typename T, typename Deleter, typename Alloc>
struct ControlBlockStand final : ControlBlock {
    T* ptr;
    Alloc alloc;
    Deleter deleter;

    ControlBlockStand(T* ptr, Deleter deleter, Alloc alloc)
        : ptr(ptr), alloc(std::move(alloc)), deleter(std::move(deleter)) {
    }

    void Dispose() override {
        deleter(ptr);
    }

    void Destroy() override {
        using RebAlloc = allocator_traits<Alloc>::template rebind_alloc<ControlBlockStand>;
        RebAlloc reb_alloc(alloc);
        this->~ControlBlockStand();
        allocator_traits<RebAlloc>::deallocate(reb_alloc, this, 1);
    }
};

template <typename T, typename Alloc>
struct ControlBlockMakeShared final : ControlBlock {
    alignas(T) std::byte arr[sizeof(T)];
    Alloc alloc;

    explicit ControlBlockMakeShared(Alloc alloc) : arr{}, alloc(alloc) {
    }

    T* GetPtr() noexcept {
        return reinterpret_cast<T*>(arr);
    }

    void Dispose() override {
        allocator_traits<Alloc>::destroy(alloc, reinterpret_cast<T*>(arr));
    }

    void Destroy() override {
        using RebAlloc =
            allocator_traits<Alloc>::template rebind_alloc<ControlBlockMakeShared>;
        RebAlloc reb_alloc(alloc);
        this->~ControlBlockMakeShared();
        allocator_traits<RebAlloc>::deallocate(reb_alloc, this, 1);
    }
};

template <typename T>
class SharedPtr {
    T* ptr_ = nullptr;
    ControlBlock* control_block_ = nullptr;

    SharedPtr(ControlBlock* control_block, T* ptr) noexcept
        : ptr_(ptr), control_block_(control_block) {
    }

    template <typename Y>
    static void InitEnableSharedFromThis(Y* ptr, ControlBlock* cb) noexcept {
        if constexpr (std::is_convertible_v<Y*, EnableSharedFromThis<Y>*>) {
            if (ptr) {
                auto* aut = static_cast<EnableSharedFromThis<Y>*>(ptr);
                ++cb->shared_count;
                SharedPtr<Y> tmp(cb, ptr);
                aut->weak_ptr_ = WeakPtr<Y>(tmp);
            }
        }
    }

public:
    void Swap(SharedPtr& other) noexcept {
        swap(control_block_, other.control_block_);
        swap(ptr_, other.ptr_);
    }

    template <typename Y, typename Deleter, typename Alloc,
              typename = enable_if_t<is_convertible_v<Y*, T*>>>
    SharedPtr(Y* ptr, Deleter deleter, Alloc alloc) : ptr_(ptr) {
        using BlockType = ControlBlockStand<Y, Deleter, Alloc>;
        using RebAlloc = allocator_traits<Alloc>::template rebind_alloc<BlockType>;
        RebAlloc reb_alloc(alloc);
        BlockType* block = allocator_traits<RebAlloc>::allocate(reb_alloc, 1);
        control_block_ = ::new (block) BlockType(ptr, std::move(deleter), std::move(alloc));
        InitEnableSharedFromThis(ptr, control_block_);
    }

    template <typename Y, typename Deleter, typename = enable_if_t<is_convertible_v<Y*, T*>>>
    SharedPtr(Y* ptr, Deleter deleter) : SharedPtr(ptr, std::move(deleter), allocator<Y>()) {
    }

    template <typename Y, typename = enable_if_t<is_convertible_v<Y*, T*>>>
    explicit SharedPtr(Y* ptr) : SharedPtr(ptr, std::default_delete<Y>(), allocator<Y>()) {
    }

    constexpr SharedPtr() noexcept = default;

    template <typename Y>
    SharedPtr(const SharedPtr<Y>& other, T* ptr) noexcept
        : ptr_(ptr), control_block_(other.control_block_) {
        if (control_block_) {
            ++control_block_->shared_count;
        }
    }

    SharedPtr(const SharedPtr& other) noexcept : SharedPtr(other, other.ptr_) {
    }

    template <typename Y, typename = std::enable_if_t<std::is_convertible_v<Y*, T*>>>
    SharedPtr(const SharedPtr<Y>& other) noexcept : SharedPtr(other, other.ptr_) {
    }

    SharedPtr(SharedPtr&& other) noexcept : ptr_(other.ptr_), control_block_(other.control_block_) {
        other.ptr_ = nullptr;
        other.control_block_ = nullptr;
    }

    template <typename Y, typename = enable_if_t<is_convertible_v<Y*, T*>>>
    SharedPtr(SharedPtr<Y>&& other) noexcept
        : ptr_(other.ptr_), control_block_(other.control_block_) {
        other.ptr_ = nullptr;
        other.control_block_ = nullptr;
    }

    SharedPtr& operator=(const SharedPtr& other) noexcept {
        SharedPtr tmp(other);
        Swap(tmp);
        return *this;
    }

    template <typename Y, typename = std::enable_if_t<std::is_convertible_v<Y*, T*>>>
    SharedPtr& operator=(const SharedPtr<Y>& other) noexcept {
        SharedPtr tmp(other);
        Swap(tmp);
        return *this;
    }

    SharedPtr& operator=(SharedPtr&& other) noexcept {
        SharedPtr tmp(std::move(other));
        Swap(tmp);
        return *this;
    }

    template <typename Y, typename = std::enable_if_t<std::is_convertible_v<Y*, T*>>>
    SharedPtr& operator=(SharedPtr<Y>&& other) noexcept {
        SharedPtr tmp(std::move(other));
        Swap(tmp);
        return *this;
    }

    ~SharedPtr() {
        if (control_block_) {
            --control_block_->shared_count;
            if (control_block_->shared_count == 0) {
                control_block_->Dispose();
                --control_block_->weak_count;
                if (control_block_->weak_count == 0) {
                    control_block_->Destroy();
                }
            }
        }
    }

    [[nodiscard]] size_t UseCount() const noexcept {
        if (control_block_) {
            return control_block_->shared_count;
        }
        return 0;
    }

    void Reset() noexcept {
        SharedPtr().Swap(*this);
    }

    template <typename Y, typename = enable_if_t<is_convertible_v<Y*, T*>>>
    void Reset(Y* ptr) {
        SharedPtr(ptr).Swap(*this);
    }

    template <typename Y, typename Deleter, typename = enable_if_t<is_convertible_v<Y*, T*>>>
    void Reset(Y* ptr, Deleter d) {
        SharedPtr(ptr, std::move(d)).Swap(*this);
    }

    template <typename Y, typename Deleter, typename Alloc,
              typename = enable_if_t<is_convertible_v<Y*, T*>>>
    void Reset(Y* ptr, Deleter d, Alloc a) {
        SharedPtr(ptr, std::move(d), std::move(a)).Swap(*this);
    }

    T* Get() const noexcept {
        return ptr_;
    }

    T& operator*() const noexcept {
        return *ptr_;
    }

    T* operator->() const noexcept {
        return ptr_;
    }

    template <typename U>
    friend class SharedPtr;
    template <typename U>
    friend class WeakPtr;
    template <typename U, typename A, typename... Args>
    friend SharedPtr<U> AllocateShared(const A&, Args&&...);
};

template <typename T, typename Alloc, typename... Args>
SharedPtr<T> AllocateShared(const Alloc& alloc, Args&&... args) {
    using ControlBlock = ControlBlockMakeShared<T, Alloc>;
    using ReboundAlloc = allocator_traits<Alloc>::template rebind_alloc<ControlBlock>;

    ReboundAlloc rebound_alloc(alloc);
    ControlBlock* control_block = allocator_traits<ReboundAlloc>::allocate(rebound_alloc, 1);
    ::new (control_block) ControlBlock(alloc);
    allocator_traits<Alloc>::construct(control_block->alloc, control_block->GetPtr(),
                                       std::forward<Args>(args)...);
    SharedPtr<T>::InitEnableSharedFromThis(control_block->GetPtr(), control_block);

    return SharedPtr<T>(control_block, control_block->GetPtr());
}

template <typename T, typename... Args>
SharedPtr<T> MakeShared(Args&&... args) {
    return AllocateShared<T>(allocator<T>(), std::forward<Args>(args)...);
}

template <typename T>
class WeakPtr {
    T* ptr_ = nullptr;
    ControlBlock* control_block_ = nullptr;

public:
    void Swap(WeakPtr& other) noexcept {
        swap(control_block_, other.control_block_);
        swap(ptr_, other.ptr_);
    }

    template <typename Y, typename = enable_if_t<is_convertible_v<Y*, T*>>>
    WeakPtr(const SharedPtr<Y>& other) noexcept
        : ptr_(other.ptr_), control_block_(other.control_block_) {
        if (control_block_) {
            ++control_block_->weak_count;
        }
    }

    constexpr WeakPtr() noexcept = default;

    WeakPtr(const WeakPtr& other) noexcept
        : ptr_(other.ptr_), control_block_(other.control_block_) {
        if (control_block_) {
            ++control_block_->weak_count;
        }
    }

    template <typename Y, typename = enable_if_t<is_convertible_v<Y*, T*>>>
    WeakPtr(const WeakPtr<Y>& other) noexcept
        : ptr_(other.ptr_), control_block_(other.control_block_) {
        if (control_block_) {
            ++control_block_->weak_count;
        }
    }

    WeakPtr(WeakPtr&& other) noexcept : ptr_(other.ptr_), control_block_(other.control_block_) {
        other.ptr_ = nullptr;
        other.control_block_ = nullptr;
    }

    template <typename Y, typename = enable_if_t<is_convertible_v<Y*, T*>>>
    WeakPtr(WeakPtr<Y>&& other) noexcept : ptr_(other.ptr_), control_block_(other.control_block_) {
        other.ptr_ = nullptr;
        other.control_block_ = nullptr;
    }

    WeakPtr& operator=(const WeakPtr& other) noexcept {
        WeakPtr tmp(other);
        Swap(tmp);
        return *this;
    }

    WeakPtr& operator=(WeakPtr&& other) noexcept {
        WeakPtr tmp(std::move(other));
        Swap(tmp);
        return *this;
    }

    ~WeakPtr() noexcept {
        if (control_block_) {
            --control_block_->weak_count;
            if (control_block_->weak_count == 0) {
                control_block_->Destroy();
            }
        }
    }

    [[nodiscard]] bool Expired() const noexcept {
        return !control_block_ || control_block_->shared_count == 0;
    }

    SharedPtr<T> Lock() const noexcept {
        if (Expired()) {
            return SharedPtr<T>();
        }
        ++control_block_->shared_count;
        return SharedPtr<T>(control_block_, ptr_);
    }

    [[nodiscard]] size_t UseCount() const noexcept {
        if (control_block_) {
            return control_block_->shared_count;
        }
        return 0;
    }

    template <typename U>
    friend class SharedPtr;
    template <typename U>
    friend class WeakPtr;
};

template <typename T>
class EnableSharedFromThis {
    WeakPtr<T> weak_ptr_;

public:
    constexpr EnableSharedFromThis() noexcept = default;

    SharedPtr<T> SharedFromThis() {
        if (weak_ptr_.Expired()) {
            throw std::bad_weak_ptr();
        }
        return weak_ptr_.Lock();
    }

    SharedPtr<const T> SharedFromThis() const {
        if (weak_ptr_.Expired()) {
            throw std::bad_weak_ptr();
        }
        return weak_ptr_.Lock();
    }

    template <typename U>
    friend class SharedPtr;
};