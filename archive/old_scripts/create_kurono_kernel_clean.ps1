# Kurono OS - Kernel-Integrated Boot System
# Creates a standalone OS with real kernel command execution

$ErrorActionPreference = "Stop"

function Write-Status {
    param([string]$Message, [string]$Status)
    if ($Status -eq "SUCCESS") {
        Write-Host "[OK] " -ForegroundColor Green -NoNewline
    } elseif ($Status -eq "ERROR") {
        Write-Host "[ERROR] " -ForegroundColor Red -NoNewline
    } else {
        Write-Host "[*] " -ForegroundColor Yellow -NoNewline
    }
    Write-Host $Message
}

Write-Host "Kurono OS Kernel-Integrated Boot Builder" -ForegroundColor Cyan
Write-Host "=========================================" -ForegroundColor Cyan
Write-Host ""

# Create boot sector with integrated kernel
$bootSector = @(
    # Boot signature and BPB
    0xEB, 0x3C, 0x90,                    # JMP short, NOP
    0x4D, 0x53, 0x57, 0x49, 0x4E, 0x34, 0x2E, 0x31, # OEM ID: "MSWIN4.1"
    0x00, 0x02,                          # Bytes per sector: 512
    0x01,                                # Sectors per cluster: 1
    0x01, 0x00,                          # Reserved sectors: 1
    0x02,                                # Number of FATs: 2
    0xE0, 0x00,                          # Root entries: 224
    0x40, 0x0B,                          # Total sectors: 2880
    0xF0,                                # Media descriptor: 0xF0
    0x09, 0x00,                          # Sectors per FAT: 9
    0x12, 0x00,                          # Sectors per track: 18
    0x02, 0x00,                          # Number of heads: 2
    0x00, 0x00, 0x00, 0x00,             # Hidden sectors: 0
    0x00, 0x00, 0x00, 0x00,             # Large total sectors: 0
    0x00,                                # Drive number: 0
    0x00,                                # Reserved: 0
    0x29,                                # Extended boot signature
    0x12, 0x34, 0x56, 0x78,             # Volume serial number
    0x4B, 0x55, 0x52, 0x4F, 0x4E, 0x4F, 0x20, 0x4F, 0x53, 0x20, 0x20, 0x20, # Volume label: "KURONO OS   "
    0x46, 0x41, 0x54, 0x31, 0x32, 0x20, 0x20, 0x20, # File system: "FAT12   "
    
    # Boot code starts here
    0xFA,                                # CLI - Clear interrupts
    0x31, 0xC0,                          # XOR AX, AX
    0x8E, 0xD8,                          # MOV DS, AX
    0x8E, 0xC0,                          # MOV ES, AX
    0x8E, 0xD0,                          # MOV SS, AX
    0xBC, 0x00, 0x7C00,                  # MOV SP, 0x7C00
    
    # Clear screen and set video mode
    0xB8, 0x03, 0x00,                    # MOV AX, 0x0003
    0xCD, 0x10,                          # INT 0x10 - Set text mode
    
    # Print Kurono OS ASCII logo
    0xBE, 0x00, 0x7E00,                  # MOV SI, logo_string
    0xE8, 0x20, 0x00,                    # CALL print_string
    
    # Print loading message
    0xBE, 0x00, 0x7F00,                  # MOV SI, loading_string
    0xE8, 0x1A, 0x00,                    # CALL print_string
    
    # Simulate loading delay
    0xB9, 0xFF, 0x00,                    # MOV CX, 0x00FF
    0x51,                                # PUSH CX
    0xE8, 0x10, 0x00,                    # CALL delay_loop
    0x59,                                # POP CX
    
    # Jump to kernel main
    0xE9, 0x00, 0x01,                    # JMP 0x0180 (kernel entry)
    
    # Print string function
    0xAC,                                # LODSB
    0x3C, 0x00,                          # CMP AL, 0
    0x74, 0x06,                          # JE print_done
    0xB4, 0x0E,                          # MOV AH, 0x0E
    0xCD, 0x10,                          # INT 0x10 - Print character
    0xEB, 0xF6,                          # JMP print_string
    0xC3,                                # RET
    
    # Delay loop function
    0x49,                                # DEC CX
    0x74, 0x02,                          # JE delay_done
    0xEB, 0xFB,                          # JMP delay_loop
    0xC3,                                # RET
    
    # Padding to 0x180 (384 bytes)
    @(0x00) * 258
)

# Kernel code with real command execution
$kernelCode = @(
    # Kernel main function
    0xFA,                                # CLI
    0x31, 0xC0,                          # XOR AX, AX
    0x8E, 0xD8,                          # MOV DS, AX
    0xBE, 0x00, 0x8000,                  # MOV SI, welcome_string
    0xE8, 0x50, 0x00,                    # CALL print_string
    
    # Initialize TTY
    0xBE, 0x00, 0x8100,                  # MOV SI, prompt_string
    0xE8, 0x4A, 0x00,                    # CALL print_string
    
    # Main command loop
    0x31, 0xC9,                          # XOR CX, CX (buffer index)
    
    # Read character
    0xB4, 0x00,                          # MOV AH, 0x00
    0xCD, 0x16,                          # INT 0x16 - Read key
    
    # Check for Enter key
    0x3C, 0x0D,                          # CMP AL, 0x0D
    0x74, 0x10,                          # JE process_command
    
    # Check for Backspace
    0x3C, 0x08,                          # CMP AL, 0x08
    0x74, 0x05,                          # JE handle_backspace
    
    # Store character in buffer
    0x88, 0x04, 0x8200,                  # MOV [buffer+CX], AL
    0x41,                                # INC CX
    0xB4, 0x0E,                          # MOV AH, 0x0E
    0xCD, 0x10,                          # INT 0x10 - Echo character
    0xEB, 0xEC,                          # JMP read_loop
    
    # Process command
    0x88, 0x04, 0x8200,                  # MOV [buffer+CX], 0 (null terminate)
    0xBE, 0x00, 0x8200,                  # MOV SI, buffer (command)
    0xE8, 0x80, 0x00,                    # CALL execute_command
    0x31, 0xC9,                          # XOR CX, CX (reset buffer)
    0xBE, 0x00, 0x8100,                  # MOV SI, prompt_string
    0xE8, 0x2E, 0x00,                    # CALL print_string
    0xEB, 0xE4,                          # JMP read_loop
    
    # Execute command function
    0x55,                                # PUSH BP
    0x89, 0xE5,                          # MOV BP, SP
    0x57,                                # PUSH DI
    0x56,                                # PUSH SI
    
    # Check for "help" command
    0xBE, 0x00, 0x8300,                  # MOV SI, cmd_help
    0xE8, 0x50, 0x00,                    # CALL string_compare
    0x74, 0x10,                          # JE cmd_help_impl
    
    # Check for "echo" command
    0xBE, 0x00, 0x8400,                  # MOV SI, cmd_echo
    0xE8, 0x48, 0x00,                    # CALL string_compare
    0x74, 0x10,                          # JE cmd_echo_impl
    
    # Check for "clear" command
    0xBE, 0x00, 0x8500,                  # MOV SI, cmd_clear
    0xE8, 0x40, 0x00,                    # CALL string_compare
    0x74, 0x05,                          # JE cmd_clear_impl
    
    # Unknown command
    0xBE, 0x00, 0x8600,                  # MOV SI, unknown_cmd
    0xE8, 0x10, 0x00,                    # CALL print_string
    0xEB, 0x20,                          # JMP execute_done
    
    # Help command implementation
    0xBE, 0x00, 0x8700,                  # MOV SI, help_output
    0xE8, 0x08, 0x00,                    # CALL print_string
    0xEB, 0x18,                          # JMP execute_done
    
    # Echo command implementation
    0xBE, 0x00, 0x8800,                  # MOV SI, echo_prefix
    0xE8, 0x00, 0x00,                    # CALL print_string
    0x5E,                                # POP SI (original command)
    0x46,                                # INC SI (skip command name)
    0x46,                                # INC SI (skip space)
    0xE8, 0xF8, 0xFF,                    # CALL print_string
    0xBE, 0x00, 0x8900,                  # MOV SI, newline
    0xE8, 0xF0, 0xFF,                    # CALL print_string
    0xEB, 0x08,                          # JMP execute_done
    
    # Clear command implementation
    0xB8, 0x03, 0x00,                    # MOV AX, 0x0003
    0xCD, 0x10,                          # INT 0x10 - Clear screen
    
    # Done
    0x5E,                                # POP SI
    0x5F,                                # POP DI
    0x5D,                                # POP BP
    0xC3,                                # RET
    
    # String compare function
    0x55,                                # PUSH BP
    0x89, 0xE5,                          # MOV BP, SP
    0x41,                                # INC CX (length)
    
    0xAC,                                # LODSB (from command)
    0x3C, 0x00,                          # CMP AL, 0
    0x74, 0x10,                          # JE compare_end
    
    0x8A, 0x1C,                          # MOV BL, [SI] (from template)
    0x3A, 0xC3,                          # CMP AL, BL
    0x75, 0x08,                          # JNE compare_fail
    
    0x46,                                # INC SI (template)
    0xEB, 0xF2,                          # JMP compare_loop
    
    0x31, 0xC0,                          # XOR AX, AX (return 0 = match)
    0xEB, 0x02,                          # JMP compare_return
    0xB8, 0x01, 0x00,                    # MOV AX, 1 (return 1 = no match)
    
    0x5D,                                # POP BP
    0xC3,                                # RET
    
    # Print string function (enhanced)
    0x55,                                # PUSH BP
    0x89, 0xE5,                          # MOV BP, SP
    0x41,                                # INC CX (length)
    
    0xAC,                                # LODSB
    0x3C, 0x00,                          # CMP AL, 0
    0x74, 0x08,                          # JE print_done
    
    0xB4, 0x0E,                          # MOV AH, 0x0E
    0xCD, 0x10,                          # INT 0x10
    0xEB, 0xF6,                          # JMP print_loop
    
    0x5D,                                # POP BP
    0xC3                                 # RET
)

# String data
$strings = @{
    logo_string = @(
        0x0A, 0x0A, 0x0A, 0x0A, 0x0A,  # Newlines
        0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x4B, 0x55, 0x52, 0x4F, 0x4E, 0x4F, 0x20, 0x4F, 0x53, 0x0A, # "       KURONO OS"
        0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x0A, # "                  "
        0x00
    )
    loading_string = @(
        0x0A, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x4C, 0x4F, 0x41, 0x44, 0x49, 0x4E, 0x47, 0x2E, 0x2E, 0x2E, 0x0A, # "    LOADING..."
        0x00
    )
    welcome_string = @(
        0x0A, 0x0A, 0x4B, 0x55, 0x52, 0x4F, 0x4E, 0x4F, 0x20, 0x4F, 0x53, 0x20, 0x76, 0x31, 0x2E, 0x30, 0x2E, 0x30, 0x0A, # "KURONO OS v1.0.0"
        0x54, 0x79, 0x70, 0x65, 0x20, 0x27, 0x68, 0x65, 0x6C, 0x70, 0x27, 0x20, 0x66, 0x6F, 0x72, 0x20, 0x63, 0x6F, 0x6D, 0x6D, 0x61, 0x6E, 0x64, 0x73, 0x0A, 0x0A, # "Type 'help' for commands"
        0x00
    )
    prompt_string = @(
        0x6B, 0x75, 0x72, 0x6F, 0x6E, 0x6F, 0x40, 0x6B, 0x75, 0x72, 0x6F, 0x6E, 0x6F, 0x73, 0x3A, 0x24, 0x20, # "kurono@kuronos:$ "
        0x00
    )
    cmd_help = @(
        0x68, 0x65, 0x6C, 0x70, 0x00      # "help"
    )
    cmd_echo = @(
        0x65, 0x63, 0x68, 0x6F, 0x00      # "echo"
    )
    cmd_clear = @(
        0x63, 0x6C, 0x65, 0x61, 0x72, 0x00 # "clear"
    )
    unknown_cmd = @(
        0x55, 0x6E, 0x6B, 0x6E, 0x6F, 0x77, 0x6E, 0x20, 0x63, 0x6F, 0x6D, 0x6D, 0x61, 0x6E, 0x64, 0x3A, 0x20, # "Unknown command: "
        0x00
    )
    help_output = @(
        0x41, 0x76, 0x61, 0x69, 0x6C, 0x61, 0x62, 0x6C, 0x65, 0x20, 0x63, 0x6F, 0x6D, 0x6D, 0x61, 0x6E, 0x64, 0x73, 0x3A, 0x0A, # "Available commands:"
        0x20, 0x20, 0x68, 0x65, 0x6C, 0x70, 0x20, 0x20, 0x20, 0x20, 0x20, 0x2D, 0x20, 0x53, 0x68, 0x6F, 0x77, 0x20, 0x74, 0x68, 0x69, 0x73, 0x20, 0x68, 0x65, 0x6C, 0x70, 0x0A, # "  help     - Show this help"
        0x20, 0x20, 0x65, 0x63, 0x68, 0x6F, 0x20, 0x20, 0x20, 0x20, 0x20, 0x2D, 0x20, 0x45, 0x63, 0x68, 0x6F, 0x20, 0x74, 0x65, 0x78, 0x74, 0x0A, # "  echo     - Echo text"
        0x20, 0x20, 0x63, 0x6C, 0x65, 0x61, 0x72, 0x20, 0x20, 0x20, 0x2D, 0x20, 0x43, 0x6C, 0x65, 0x61, 0x72, 0x20, 0x73, 0x63, 0x72, 0x65, 0x65, 0x6E, 0x0A, # "  clear    - Clear screen"
        0x00
    )
    echo_prefix = @(
        0x45, 0x63, 0x68, 0x6F, 0x3A, 0x20, # "Echo: "
        0x00
    )
    newline = @(
        0x0A, 0x00                          # "\n"
    )
    buffer = @(0x00) * 64  # Command buffer
}

try {
    Write-Status "Creating Kurono OS kernel-integrated boot image..." "INFO"
    
    # Create the boot image
    $bootImage = New-Object byte[] 1474560  # 1.44MB floppy
    
    # Copy boot sector
    for ($i = 0; $i -lt $bootSector.Count; $i++) {
        $bootImage[$i] = $bootSector[$i]
    }
    
    # Copy kernel code
    $kernelOffset = 0x180  # 384 bytes
    for ($i = 0; $i -lt $kernelCode.Count; $i++) {
        $bootImage[$kernelOffset + $i] = $kernelCode[$i]
    }
    
    # Copy strings
    $stringOffset = 0x7E00  # String data area
    foreach ($stringName in $strings.Keys) {
        $stringData = $strings[$stringName]
        for ($i = 0; $i -lt $stringData.Count; $i++) {
            $bootImage[$stringOffset + $i] = $stringData[$i]
        }
        $stringOffset += $stringData.Count
    }
    
    # Write boot image
    [System.IO.File]::WriteAllBytes("kurono_os_kernel.img", $bootImage)
    Write-Status "Boot image created successfully" "SUCCESS"
    
    Write-Host ""
    Write-Host "Kurono OS Kernel-Integrated System Features:" -ForegroundColor Cyan
    Write-Host "  - help, echo, clear commands" -ForegroundColor White
    Write-Host "  - Professional boot experience" -ForegroundColor White
    Write-Host "  - Working command execution" -ForegroundColor White
    Write-Host "  - Integrated kernel functionality" -ForegroundColor White
    Write-Host ""
    Write-Host "File: D:\Kurono\Kurnon OS\kurono_os_kernel.img" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "Testing Kurono OS with Real Kernel Commands..." -ForegroundColor Yellow
    Write-Host "Try these commands:" -ForegroundColor Green
    Write-Host "  - help (shows all commands)" -ForegroundColor White
    Write-Host "  - echo hello" -ForegroundColor White
    Write-Host "  - clear (clears screen)" -ForegroundColor White
    Write-Host ""
    
    # Test with QEMU
    qemu-system-x86_64 -fda kurono_os_kernel.img -boot order=a -m 256M -vga std
    
} catch {
    Write-Status "Error: $_" "ERROR"
    exit 1
}