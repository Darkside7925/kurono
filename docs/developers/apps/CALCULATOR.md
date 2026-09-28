# Calculator

`src/apps/calculator.cpp` and `calculator.h`. Keypad plus a small expression
evaluator.

Does `+`, `-`, `*`, `/`, parentheses, order of operations, decimals, backspace and
CE/C. Takes keyboard entry as well as button clicks.

## How it evaluates

Input accumulates as a string. Hit `=` and a recursive descent parser evaluates it.
Results print with up to 10 significant digits.

## Input

Button clicks are hit-tested against the button grid. Keyboard digits and operators
come through the same key dispatch the terminal uses, in the window manager's input
routing.

## Files

- `src/ui/gui.cpp` button rendering
- `src/ui/desktop.cpp` `LaunchCalculator()` entry point
