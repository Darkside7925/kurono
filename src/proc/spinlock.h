#pragma once
//  kurono os  -  spinlock primitive (header-only)
//
//  Lightweight spinlock with optional cli/sti save+restore for protecting
//  shared kernel state under preemptive multitasking.  The lock is a
//  uint32_t test-and-set word manipulated with `lock; xchg`, which is the
//  cheapest atomic test-and-set on x86_64.
//
//  Two flavours of acquire:
//    * Spinlock::Lock()    -  bare spin (caller already manages IF)
//    * Spinlock::LockIrqSave(out_flags)  -  disables IF, returns prior state
//
//  RAII helper SpinLockGuard automatically saves/restores rflags so that
//  the same critical section can run with or without interrupts already
//  disabled (e.g. nested IRQ handlers vs process context).

#include "../kernel/types.h"

class Spinlock {
public:
    constexpr Spinlock() : value_(0), owner_pid_(0) {}

    inline void Lock() {
        for (;;) {
            uint32_t expected = 0;
            if (__atomic_compare_exchange_n(&value_, &expected, 1u, false,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                return;
            }
            while (__atomic_load_n(&value_, __ATOMIC_RELAXED) != 0) {
                __asm__ __volatile__("pause");
            }
        }
    }

    inline bool TryLock() {
        uint32_t expected = 0;
        return __atomic_compare_exchange_n(&value_, &expected, 1u, false,
                                           __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
    }

    inline void Unlock() {
        __atomic_store_n(&value_, 0u, __ATOMIC_RELEASE);
    }

    // IRQ-save variant: writes the prior IF state into out_flags so the
    // matching UnlockIrqRestore() can correctly restore (or leave clear).
    inline void LockIrqSave(uint64_t* out_flags) {
        uint64_t flags;
        __asm__ __volatile__("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
        Lock();
        if (out_flags) *out_flags = flags;
    }

    inline void UnlockIrqRestore(uint64_t flags) {
        Unlock();
        if (flags & 0x200ULL) {
            __asm__ __volatile__("sti" ::: "memory");
        }
    }

    inline bool IsLocked() const {
        return __atomic_load_n(&value_, __ATOMIC_ACQUIRE) != 0;
    }

private:
    volatile uint32_t value_;
    volatile uint32_t owner_pid_; // diagnostic only  -  set by RAII guard
};

// RAII helper.  Disables interrupts and acquires the spinlock on
// construction, restores rflags on destruction.  Safe to nest because
// each guard saves the prior IF state independently.
class SpinLockGuard {
public:
    explicit SpinLockGuard(Spinlock& lock) : lock_(&lock), flags_(0) {
        lock_->LockIrqSave(&flags_);
    }
    ~SpinLockGuard() {
        if (lock_) lock_->UnlockIrqRestore(flags_);
    }
    SpinLockGuard(const SpinLockGuard&)            = delete;
    SpinLockGuard& operator=(const SpinLockGuard&) = delete;

private:
    Spinlock* lock_;
    uint64_t  flags_;
};

// RAII acquire without cli/sti  -  for long critical sections on the BSP
// where IRQ handlers must keep firing (PIT, device IRQs).  Only safe if
// no interrupt or exception path ever tries to take the *same* lock
// (would deadlock on re-entrancy).  Kernel-process mutual exclusion only.
class SpinLockCpuGuard {
public:
    explicit SpinLockCpuGuard(Spinlock& lock) : lock_(&lock) { lock_->Lock(); }
    ~SpinLockCpuGuard() {
        if (lock_) lock_->Unlock();
    }
    SpinLockCpuGuard(const SpinLockCpuGuard&)            = delete;
    SpinLockCpuGuard& operator=(const SpinLockCpuGuard&) = delete;

private:
    Spinlock* lock_;
};
