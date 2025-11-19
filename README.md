# Kurono OS - Unified Hybrid Kernel

Kurono OS is a revolutionary hybrid kernel system that seamlessly integrates Linux, Windows, and native Kurono command environments into a single, unified operating system.

## Features

### 🌐 Multi-Environment Integration
- **Linux Subsystem**: Full GNU utilities and POSIX compliance
- **Windows Subsystem**: Native PE executable support and PowerShell integration
- **Kurono Native**: Custom command language (KCL) and native tools

### 🔧 Advanced Capabilities
- **Native PE Execution**: Run Windows .exe files without Wine or translation layers
- **Command Conflict Resolution**: Intelligent handling when commands exist in multiple environments
- **Cross-Environment Pipelining**: Pipe commands between different subsystems
- **Advanced Security**: SUPR (Super User) privilege escalation system
- **Package Management**: Unified package system for all environments

### 🛡️ Security Features
- **SUPR Mode**: Kurono's equivalent of sudo/su with timeout protection
- **Permission System**: Fine-grained access control across all environments
- **User Management**: Multi-user support with role-based access
- **Secure Authentication**: SHA-256 password hashing and verification

## Architecture

### Core Components

1. **Kernel Core** (`kernel_core/`)
   - Central command registry and execution engine
   - Environment switching and context management
   - Cross-subsystem communication

2. **Linux Bridge Layer** (`linux_bridge_layer/`)
   - GNU utilities integration
   - POSIX filesystem compatibility
   - Linux command execution

3. **Windows Bridge Layer** (`windows_bridge_layer/`)
   - PE loader and execution engine
   - PowerShell integration
   - Windows registry simulation

4. **KCL Interpreter** (`kcl_interpreter/`)
   - Kurono Command Language parser and executor
   - Scripting capabilities
   - Variable management and flow control

5. **Conflict Resolver** (`conflict_resolver/`)
   - Command ambiguity detection
   - User preference management
   - Automatic resolution strategies

6. **Security SUPR Engine** (`security_supr_engine/`)
   - User authentication and management
   - Permission system
   - SUPR privilege escalation

7. **Package Manager** (`package_manager/`)
   - Unified package installation/removal
   - Repository management
   - Dependency resolution

## Building

### Prerequisites
- GCC compiler
- OpenSSL development libraries
- Windows SDK (for PE support)
- Linux development headers

### Compilation
```bash
make clean
make all
make install
```

### Installation
The build system will install Kurono OS to:
- Executables: `D:\Kurono\KuronoOS\Build_Files`
- Source code: `D:\Important\Kurono`

## Usage

### Starting Kurono OS
```bash
./kurono_os
```

### Basic Commands
- `help` - Show available commands
- `version` - Display version information
- `env` - Show current environment
- `switch <environment>` - Switch between environments (linux/windows/kurono)
- `supr` - Enable SUPR (root) mode

### Command Resolution
When a command exists in multiple environments, Kurono OS will prompt:
```
> dir
[System Alert] Command 'dir' exists in multiple environments:
1) /bin/dir          (Linux)
2) dir.ps1           (PowerShell)
3) dir.kc            (Kurono)
Enter selection (1-3):
```

### SUPR Mode
SUPR (Super User) is Kurono's privilege escalation system:
```
Kurono OS> supr
Enter admin password: *****
SUPR mode enabled. You now have root privileges.
SUPR mode will expire in 15 minutes.
```

### Package Management
- `install <package>` - Install a package
- `remove <package>` - Remove a package
- `list` - List installed packages
- `search <query>` - Search for packages

### KCL Scripting
Execute KCL scripts:
```bash
kcl script.kcl
```

## Command Environments

### Linux Environment
- Standard GNU utilities (ls, cat, grep, etc.)
- POSIX shell compatibility
- Linux filesystem structure

### Windows Environment
- Windows commands (dir, copy, etc.)
- PowerShell cmdlets
- PE executable support
- Registry operations

### Kurono Environment
- KCL interpreter
- Native Kurono commands
- Cross-environment utilities
- System management tools

## Security

### User Management
- Multi-user support with role-based permissions
- SHA-256 password hashing
- Session management

### Permission System
- File and resource permissions
- Environment-specific access controls
- SUPR privilege escalation with timeout

### SUPR Security
- Time-limited privilege escalation
- Audit logging
- Secure password input

## Testing

Run the comprehensive test suite:
```bash
make test
```

Or run individual tests:
```bash
./kurono_os --test-kernel
./kurono_os --test-linux
./kurono_os --test-windows
./kurono_os --test-kcl
./kurono_os --test-conflicts
./kurono_os --test-security
./kurono_os --test-packages
./kurono_os --test-integration
```

## File Structure
```
KuronoOS/
├── kernel_core/           # Core kernel functionality
├── linux_bridge_layer/    # Linux subsystem integration
├── windows_bridge_layer/  # Windows subsystem integration
├── kcl_interpreter/     # Kurono Command Language interpreter
├── conflict_resolver/     # Command conflict resolution
├── security_supr_engine/  # Security and privilege management
├── package_manager/       # Package management system
├── Commands/             # Native Kurono commands
├── Tests/               # Test suite
└── Build/               # Build output directory
```

## Contributing

1. Fork the repository
2. Create a feature branch
3. Implement your changes
4. Add comprehensive tests
5. Submit a pull request

## License

Kurono OS is released under the MIT License. See LICENSE file for details.

## Support

For support and questions:
- Documentation: `help` command in Kurono OS
- Issues: Report on the project repository
- Community: Join our development forums

---

**Kurono OS** - Unifying the operating system landscape with hybrid kernel technology.