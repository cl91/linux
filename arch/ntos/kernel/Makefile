extra-y := vmlinux.lds

KASAN_SANITIZE_init.o := n
KASAN_SANITIZE_stacktrace.o := n

obj-y = setup.o sched.o irq.o kthread.o time.o printk.o \
	panic.o cpu.o stacktrace.o ratelimit.o main.o module.o \
	../../../kernel/sched/wait.o ../../../kernel/sched/swait.o \
	../../../kernel/sched/completion.o ../../../kernel/sched/wait_bit.o \
	../../../kernel/notifier.o ../../../kernel/workqueue.o \
	../../../kernel/locking/semaphore.o ../../../kernel/locking/mutex.o \
	../../../kernel/locking/rwsem.o ../../../kernel/locking/percpu-rwsem.o \
	../../../kernel/irq/dummychip.o ../../../kernel/irq/irqdesc.o \
	../../../kernel/irq/handle.o ../../../kernel/irq/resend.o \
	../../../kernel/irq/chip.o ../../../kernel/irq/msi.o \
	../../../kernel/irq/irqdomain.o ../../../kernel/irq/spurious.o \
	../../../kernel/irq/manage.o ../../../kernel/task_work.o \
	../../../kernel/rcu/update.o ../../../kernel/rcu/sync.o \
	../../../kernel/rcu/tiny.o ../../../kernel/rcu/srcutiny.o \
	../../../kernel/time/time.o ../../../kernel/time/timer.o \
	../../../kernel/time/timeconv.o ../../../kernel/time/timecounter.o \
	../../../kernel/time/sleep_timeout.o ../../../kernel/kthread.o \
	../../../kernel/module/main.o ../../../kernel/module/strict_rwx.o \
	../../../kernel/params.o ../../../kernel/resource.o \
	../../../kernel/dma/mapping.o ../../../kernel/dma/direct.o \
	../../../kernel/async.o ../../../kernel/smpboot.o \
	../../../kernel/pid.o ../../../kernel/iomem.o
obj-$(CONFIG_RT_MUTEXES) += ../../../kernel/locking/rtmutex_api.o
obj-$(TARGET_ARCH_X86) += ../../x86/kernel/module.o
obj-$(TARGET_ARCH_ARM64) += ../../arm64/kernel/module.o ../../arm64/kernel/module-plts.o \
	../../arm64/lib/insn.o
CFLAGS_../../arm64/kernel/module.o += -I$(srctree)/arch/arm64/include
CFLAGS_../../arm64/kernel/module-plts.o += -I$(srctree)/arch/arm64/include
CFLAGS_../../arm64/lib/insn.o += -I$(srctree)/arch/arm64/include
