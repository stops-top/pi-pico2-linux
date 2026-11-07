include $(sort $(wildcard $(BR2_EXTERNAL)/package/*/*.mk))

# uClibc >= 1.0.59 builds libm/riscv32/f*.c when UCLIBC_HAS_FENV is set, and
# buildroot force-enables that symbol whenever BR2_SOFT_FLOAT is off -- a symbol
# no RISC-V configuration can select.  Hazard3 is RV32IMAC with neither F nor
# Zfinx, so frrm/fsrm/frflags/fsflags and the fcsr/frm/fflags CSRs do not exist
# and binutils rejects them ("unrecognized opcode `frrm a0'").  Keep buildroot's
# non-soft-float settings minus the floating-point environment; this variable is
# expanded from UCLIBC_KCONFIG_FIXUP_CMDS at build time and external.mk is read
# after package/*/*.mk, so the redefinition wins.
define UCLIBC_FLOAT_CONFIG
	$(call KCONFIG_ENABLE_OPT,UCLIBC_HAS_FPU)
	$(call KCONFIG_ENABLE_OPT,UCLIBC_HAS_FLOATS)
	$(call KCONFIG_DISABLE_OPT,UCLIBC_HAS_FENV)
endef
