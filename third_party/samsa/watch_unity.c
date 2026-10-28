/* SPDX-License-Identifier: Apache-2.0 */
/* Compile the interpreter together so the host can inline its hot helpers. */
#include "samsa/src/alu.c"
#include "samsa/src/mem.c"
#include "samsa/src/decode16.c"
#include "samsa/src/decode32.c"
#include "samsa/src/samsa.c"
