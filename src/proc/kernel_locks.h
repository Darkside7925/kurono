#pragma once
//  kurono os - global kernel spinlocks
//
//  Lockable shared resources protected by their own Spinlock.  Each lock
//  is declared here and defined exactly once in scheduler.cpp so any
//  subsystem can grab it via SpinLockGuard.
//
//  Locks:
//    g_net_lock    - TCPStack socket table, ARP cache, pending queue
//    g_input_lock  - keyboard / mouse ring buffers
//    g_vfs_lock    - KVFS read / write operations
//    g_fb_lock     - framebuffer swap and Graphics::DrawPixel paths
//
//  Prefer SpinLockCpuGuard for these in kernel_processes.cpp: holding
//  SpinLockGuard (cli) across Render/TCP tick would stall IRQs and feel
//  like random input / timer lag.
//    g_audio_lock  - mixer stream slots
//    g_log_lock    - RuntimeLog buffers (added by LoggingProcess flush)

#include "spinlock.h"

extern Spinlock g_net_lock;
extern Spinlock g_input_lock;
extern Spinlock g_vfs_lock;
extern Spinlock g_fb_lock;
extern Spinlock g_audio_lock;
extern Spinlock g_log_lock;
