#!/bin/bash
# Test script for Kurono OS

echo "Kurono OS Test Suite"
echo "===================="

# Test 1: Basic kernel functionality
echo "Test 1: Kernel initialization"
./kurono_os --test-kernel

# Test 2: Linux bridge
echo "Test 2: Linux bridge functionality"
./kurono_os --test-linux

# Test 3: Windows bridge
echo "Test 3: Windows bridge functionality"
./kurono_os --test-windows

# Test 4: KCL interpreter
echo "Test 4: KCL interpreter"
./kurono_os --test-kcl

# Test 5: Conflict resolution
echo "Test 5: Conflict resolution"
./kurono_os --test-conflicts

# Test 6: Security engine
echo "Test 6: Security engine"
./kurono_os --test-security

# Test 7: Package manager
echo "Test 7: Package manager"
./kurono_os --test-packages

# Test 8: Integration
echo "Test 8: Full integration test"
./kurono_os --test-integration

echo "All tests completed!"