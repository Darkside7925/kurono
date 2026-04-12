# Browser App

`src/apps/browser.cpp` and `browser.h` implement the built-in web browser shell.

## 1. What it is

The browser app is a minimal HTTP client UI. It provides:

- An address bar for typing URLs
- A render area that displays fetched content
- Basic navigation: back, forward, refresh buttons

## 2. Network integration

URL fetches go through the TCP/IP stack in `src/net/tcpip.cpp`. The browser opens a TCP connection to the target host on port 80, sends a minimal HTTP/1.1 GET request, reads the response headers and body, and passes the body to the renderer.

HTTPS is not currently implemented. HTTP only.

## 3. Content rendering

The browser does not implement a full HTML/CSS engine. It renders the response body as plain text with basic formatting  -  newlines, word wrap, and simple heading detection. The display updates line by line as data arrives.

## 4. Limitations

This browser is a capability demonstration, not a production-ready renderer. It works for simple HTTP text pages and basic REST endpoints. It does not support JavaScript, CSS, images (beyond basic deocded images), or modern web standards.

## 5. Related files

- `src/net/tcpip.cpp`  -  TCP connection used by the browser
- `src/ui/desktop.cpp`  -  `LaunchBrowser()` entry point
