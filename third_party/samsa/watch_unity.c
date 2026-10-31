/* SPDX-License-Identifier: Apache-2.0 */
/* Compile the interpreter together so the host can inline its hot helpers. */
// The interpreter sources must compile in this order.
// clang-format off
#include "samsa/src/alu.c"
#include "samsa/src/mem.c"
#include "samsa/src/decode16.c"
#include "samsa/src/decode32.c"
#include "samsa/src/samsa.c"
// clang-format on
