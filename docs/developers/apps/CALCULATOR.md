# Calculator App

`src/apps/calculator.cpp` and `calculator.h` implement a desktop calculator.

## 1. What it does

The calculator is a simple expression evaluator with a numeric keypad UI. It supports:

- Basic arithmetic: `+`, `-`, `*`, `/`
- Parentheses and order of operations
- Decimal input
- Backspace and clear (CE/C)
- Keyboard number entry in addition to button clicks

## 2. Expression evaluation

Input is accumulated as a string. When `=` is pressed, the expression string is evaluated by a small recursive descent parser. Results are displayed with up to 10 significant digits.

## 3. Input handling

Button clicks are hit-tested against the button grid. Keyboard digits and operators are handled by the terminal-style key dispatch path in the window manager's input routing.

## 4. Related files

- `src/ui/gui.cpp`  -  button rendering
- `src/ui/desktop.cpp`  -  `LaunchCalculator()` entry point
