# Beginner Documentation

This section is for readers who want to understand the project without starting from the deepest hardware details on day one.

## Start Here

1. Read `FIRST_STEPS.md`
2. Read `PROJECT_TOUR.md`
3. Read `BUILD_AND_RUN.md`

## What Kurono OS Is

Kurono OS is a bare metal x86 64 operating system with a desktop, built in applications, a mixed shell, a Linux style subsystem, a virtual machine stack, installer support, and a large hardware facing driver set.

That sounds bigger than it feels once the project is broken down into routes.

The beginner set focuses on those routes.

1. How the system boots
2. How the desktop appears
3. How the shell is wired
4. Which folders matter first
5. How to build and test safely

## The Three Ideas to Keep in Mind

### First idea

`src/kernel/kurono_kernel.cpp` is the real boot story.

### Second idea

`src/shell/shell.cpp` is the real command story.

### Third idea

`src/ui/desktop.cpp` and `src/ui/window_manager.cpp` are the real user interface story.

If those four files make sense, the repository becomes much easier to navigate.
