// kld-acpi: shared declarations between the ACPICA OSL and the front end.
// end (satoru)

#ifndef KLD_ACPI_H
#define KLD_ACPI_H

void kld_phys_add(acpi_physical_address base, void *host, acpi_size len);

extern int kld_osl_broker_mode;
extern acpi_physical_address kld_osl_rsdp;
extern int kld_acpi_quiet;
extern unsigned long kld_sim_pages, kld_port_reads, kld_port_writes, kld_pci_reads, kld_pci_writes;

#endif
// end (satoru)
