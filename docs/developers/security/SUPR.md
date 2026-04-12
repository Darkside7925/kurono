# Privilege System (SUPR)

`src/security/supr.cpp` and `supr.h` implement the Kurono privilege management layer.

## 1. What it does

SUPR (Super Privilege) manages privilege escalation requests. When a command or subsystem needs elevated access  -  for example, mounting a disk, modifying system config, or killing another user's process  -  it calls through the SUPR layer.

## 2. Privilege model

Kurono has two privilege levels at the software layer:

- **User**  -  standard unprivileged operations
- **Root**  -  all operations permitted

The current default is a single user in the "root" category because multiple user accounts with isolation are not yet enforced at the memory protection level. SUPR enforces behavioral limits even when hardware memory isolation is not present.

## 3. sudo integration

The `sudo` shell command calls SUPR to request a privilege context for the next command. If the user has the required credential (password or token), SUPR grants the elevated context for the duration of that command.

## 4. Future direction

When full user-space process isolation is implemented with hardware ring separation, SUPR will be the bridge between user-mode syscall requests and kernel-mode handlers. The current implementation is a placeholder that enforces the concept without full hardware backing.

## 5. Related files

- `src/shell/linux_cmds.cpp`  -  `sudo` command calls SUPR
- `src/system/user_mgmt.cpp`  -  user credentials stored and checked here
