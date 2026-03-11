#!/bin/bash
make clean;make NCPU=1

echo
echo -e "\033[1;34m>>> spike ./obj/riscv-pke bin/app_print_backtrace\033[0m"
spike ./obj/riscv-pke bin/app_print_backtrace
echo

echo -e "\033[1;34m>>> spike ./obj/riscv-pke bin/app_errorline\033[0m"
spike ./obj/riscv-pke bin/app_errorline
echo

echo -e "\033[1;34m>>> spike ./obj/riscv-pke bin/app_sum_sequence\033[0m"
spike ./obj/riscv-pke bin/app_sum_sequence
echo

echo -e "\033[1;34m>>> spike ./obj/riscv-pke bin/app_singlepageheap\033[0m"
spike ./obj/riscv-pke bin/app_singlepageheap
echo

echo -e "\033[1;34m>>> spike ./obj/riscv-pke bin/app_wait\033[0m"
spike ./obj/riscv-pke bin/app_wait
echo

echo -e "\033[1;34m>>> spike ./obj/riscv-pke bin/app_semaphore\033[0m"
spike ./obj/riscv-pke bin/app_semaphore
echo

echo -e "\033[1;34m>>> spike ./obj/riscv-pke bin/app_cow\033[0m"
spike ./obj/riscv-pke bin/app_cow
echo

echo -e "\033[1;34m>>> spike ./obj/riscv-pke bin/app_relativepath\033[0m"
spike ./obj/riscv-pke bin/app_relativepath
echo

echo -e "\033[1;34m>>> spike ./obj/riscv-pke bin/app_exec\033[0m"
spike ./obj/riscv-pke bin/app_exec
echo

echo -e "\033[1;34m>>> spike ./obj/riscv-pke bin/app_shell\033[0m"
spike ./obj/riscv-pke bin/app_shell
echo

make clean;make NCPU=2

echo -e "\033[1;34m>>> spike -p2 ./obj/riscv-pke bin/app0 bin/app1\033[0m"
spike -p2 ./obj/riscv-pke bin/app0 bin/app1
echo

echo -e "\033[1;34m>>> spike -p2 ./obj/riscv-pke bin/app_alloc0 bin/app_alloc1\033[0m"
spike -p2 ./obj/riscv-pke bin/app_alloc0 bin/app_alloc1
echo