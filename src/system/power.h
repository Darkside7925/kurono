#pragma once
#include "../kernel/types.h"

//  kurono os - one power manager for every reboot / shutdown / sleep request.
//
//  the control center, the shell commands, the start menu and the acpi power
//  button all queue their action here. one worker kernel process runs the
//  sequence off the gui thread: shutdown film, SIGTERM to every ring-3
//  program with a bounded wait (so claude code writes its session), a
//  persisted user-data save with a failure prompt, kinit target shutdown, a
//  disk-log flush, then the platform reset / power-off. (satoru)
namespace PowerManager {

enum Action : uint8_t {
    ACTION_NONE = 0,
    ACTION_SLEEP,
    ACTION_RESTART,
    ACTION_SHUTDOWN
};

// spawn the worker (idempotent) and arm the acpi power button. called once
// from kernel_main after the scheduler is up. (satoru)
void Start();

// queue an action. returns false when an action is already queued/running.
// origin is only for the [power] log line. (satoru)
bool Request(Action action, const char* origin);

// true while a request is queued or the sequence is running. (satoru)
bool Busy();

// one background pass: poll the acpi power button, run a queued action and
// check the autosave cadence. called from the power worker and, because that
// kernel proc can be starved under a firefox cpu load, from the disk log
// worker as well. only one caller wins each action / save. (satoru)
void BackgroundTick();

// starvation valve hook: true when the worker should get a slice now (a queued
// action, or the 1 s poll that services the acpi button and autosave).
// PumpKick hands it one and returns true when that happened. called from the
// user syscall path with the kls released, where a kernel proc can safely run
// even while firefox starves the normal scheduler. (satoru)
bool PumpKickDue();
bool PumpKick();

// log a [power] line and queue shutdown (the acpi power button uses this). (satoru)
void NotifyPowerButton();

// test hooks (kurono.power.test=...). parsed once before Start(). (satoru)
void SetTest(const char* spec, uint32_t delay_ms);

}  // namespace PowerManager

// end (satoru)
