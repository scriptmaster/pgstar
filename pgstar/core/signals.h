#ifndef PGSTAR_CORE_SIGNALS_H
#define PGSTAR_CORE_SIGNALS_H

#include "postgres.h"
#include "storage/latch.h"

extern volatile sig_atomic_t pgstar_got_sigterm;

void pgstar_register_sigterm(void);

#endif /* PGSTAR_CORE_SIGNALS_H */

