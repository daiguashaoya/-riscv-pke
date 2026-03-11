make clean;make
spike ./obj/riscv-pke bin/app_print_backtrace
spike ./obj/riscv-pke bin/app_errorline

spike ./obj/riscv-pke bin/app_sum_sequence
spike ./obj/riscv-pke bin/app_singlepageheap

spike ./obj/riscv-pke bin/app_wait
spike ./obj/riscv-pke bin/app_semaphore
spike ./obj/riscv-pke bin/app_cow

spike ./obj/riscv-pke bin/app_relativepath
spike ./obj/riscv-pke bin/app_exec
spike ./obj/riscv-pke bin/app_shell