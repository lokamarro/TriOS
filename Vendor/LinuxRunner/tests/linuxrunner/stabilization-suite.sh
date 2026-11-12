#!/bin/sh
# Sources must be copied to the exchange folder of a disposable guest fixture.
set -eu
cd /home/mobile/Downloads
for name in unix_credentials unix_explicit_credentials fchownat_empty protnone_arena browser_readiness shm_namespace vector_fixed scalar_fcvt scalar_fcvt_flags thread_clock brk_accounting parent_process futex_bitset futex_pi_atomic futex_pi_contended futex_pi_edges futex_pi_stress oobinline; do
 printf 'STABILIZATION_TEST: %s\n' "$name"
 gcc "regress_$name.c" -O2 -pthread -lm -o "regress_$name"
 "./regress_$name"
done
echo STABILIZATION_SUITE_PASS_18
