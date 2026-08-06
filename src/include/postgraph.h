/*
* PostGraph
 * Copyright (C) 2026 by PostGraph
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */


#ifndef NP_POSTGRAPH_H
#define NP_POSTGRAPH_H

#include "postgres.h"

#include "access/tableam.h" 
#include "fmgr.h"
#include "utils/guc.h"
#include "utils/elog.h"

typedef enum
{
    ANNOT_MIGRATION_STRICT,
    ANNOT_MIGRATION_DROP,
    ANNOT_MIGRATION_AUTO
} AnnotMigrationMode;

int np_annotation_migration_mode = ANNOT_MIGRATION_STRICT;

static const struct config_enum_entry annotation_migration_options[] = {
    {"strict", ANNOT_MIGRATION_STRICT, false},
    {"drop",   ANNOT_MIGRATION_DROP,   false},
    {"auto",   ANNOT_MIGRATION_AUTO,   false},
    {NULL, 0, false}
};

#endif
