// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#ifndef _QUETZ_INSTRUMENT_H
#define _QUETZ_INSTRUMENT_H

#include <glib.h>
extern "C" {
#include "qemu-plugin.h"
}

void register_plugin_callbacks(qemu_plugin_id_t id);

#endif // _QUETZ_INSTRUMENT_H
