#
# Makefile for the linux kernel.
#

CPPFLAGS_vmlinux.lds	:= -DTEXT_OFFSET=$(TEXT_OFFSET)
AFLAGS_head.o		:= -DTEXT_OFFSET=$(TEXT_OFFSET)

ifdef CONFIG_FUNCTION_TRACER
CFLAGS_REMOVE_ftrace.o = -pg
CFLAGS_REMOVE_insn.o = -pg
CFLAGS_REMOVE_return_address.o = -pg
endif

# Object file lists.
arm64-obj-y		:= cputable.o debug-monitors.o entry.o irq.o fpsimd.o	\
			   entry-fpsimd.o process.o ptrace.o setup.o signal.o	\
			   sys.o stacktrace.o time.o traps.o io.o vdso.o	\
			   hyp-stub.o psci.o return_address.o

arm64-obj-$(CONFIG_AARCH32_EL0)	+= sys32.o kuser32.o \
												   sys_compat.o
arm64-obj-$(CONFIG_ARM64_ILP32)	+= sys_ilp32.o
arm64-obj-$(CONFIG_COMPAT)		+= signal32.o
arm64-obj-$(CONFIG_MODULES)		+= arm64ksyms.o module.o
arm64-obj-$(CONFIG_SMP)			+= smp.o smp_spin_table.o smp_psci.o
arm64-obj-$(CONFIG_HW_PERF_EVENTS)	+= perf_event.o
arm64-obj-$(CONFIG_DYNAMIC_FTRACE)	+= ftrace.o insn.o
arm64-obj-$(CONFIG_FUNCTION_GRAPH_TRACER) += ftrace.o insn.o
arm64-obj-$(CONFIG_HAVE_HW_BREAKPOINT)+= hw_breakpoint.o
arm64-obj-$(CONFIG_EARLY_PRINTK)	+= early_printk.o
arm64-obj-$(CONFIG_KGDB)		+= kgdb.o

obj-y					+= $(arm64-obj-y) vdso/
obj-$(CONFIG_ARM64_ILP32)		+= vdsoilp32/
obj-m					+= $(arm64-obj-m)
head-y					:= head.o
extra-y					:= $(head-y) vmlinux.lds

# vDSO - this must be built first to generate the symbol offsets
$(call objectify,$(arm64-obj-y)): $(obj)/vdso/vdso-offsets.h
$(obj)/vdso/vdso-offsets.h: $(obj)/vdso

ifeq ($(CONFIG_ARM64_ILP32),y)
$(call objectify,$(arm64-obj-y)): $(obj)/vdsoilp32/vdso-ilp32-offsets.h
$(obj)/vdsoilp32/vdso-ilp32-offsets.h: $(obj)/vdsoilp32
endif
