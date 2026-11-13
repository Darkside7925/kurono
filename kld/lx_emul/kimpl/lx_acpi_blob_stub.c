// weak empty table-blob fallbacks for ACPICA families that link no blob.
//
// The real i2chid family reads the firmware tables through the kernel broker,
// so it links no generated table array. These weak symbols keep
// lx_acpi_tables.c's blob provider linkable (it then reports "no table blob");
// a simulated family's strong array wins at link time. They live in a separate
// translation unit so the compiler cannot constant-fold the ntables check in
// lx_acpi_tables.c to 0 (which silently broke the simulated family once).
// (satoru)

#include "lx_acpi.h"

__attribute__((weak)) const unsigned char lx_acpi_blob_data[1] = { 0 };
__attribute__((weak)) const struct lx_acpi_blob_table lx_acpi_blob_tables[1] = { { 0, 0 } };
__attribute__((weak)) const unsigned int lx_acpi_blob_ntables = 0;

// end (satoru)
