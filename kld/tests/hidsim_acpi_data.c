// hidsim references acpi_gbl_FADT through gpiolib-acpi-core but links no
// ACPICA (the real families define it in acpica/utglobal.o). Deterministic
// zeroed storage beats the old function stub whose code bytes were read as a
// table (C5.7b). hidsim-only so it cannot collide with the ACPICA definition.
// end (satoru)

#include <linux/kernel.h>
#include <linux/acpi.h>

// the real ACPICA FADT type, zeroed: hidsim never enables ACPI hardware and
// its gpiolib-acpi reference only reads boot flags that are absent here (the
// same "no firmware ACPI" state the pre-ACPI shim implemented). (satoru)
struct acpi_table_fadt acpi_gbl_FADT;

// end (satoru)
