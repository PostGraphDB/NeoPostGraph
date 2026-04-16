# NeoPostGraph is a PostgreSQL extension for graph database capabilities.
#    Copyright (C) 2026  NeoPostGraph Contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as
# published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

MODULE_big = neopostgraph

OBJS = src/backend/neopostgraph.o \
       src/backend/catalog/np_catalog.o \
       src/backend/catalog/np_graph.o \
	src/backend/catalog/np_label.o \
	src/backend/utils/np_cache.o

EXTENSION = neopostgraph

DATA = neopostgraph--0.1.0.sql

REGRESS = graph \
          label \
          neopostgraph

srcdir=`pwd`
PG_LIBS += -ltree
.PHONY:all

all: neopostgraph--0.1.0.sql

ag_regress_dir = $(srcdir)/regress
REGRESS_OPTS = --load-extension=btree_gist --load-extension=ltree --load-extension=neopostgraph --inputdir=$(ag_regress_dir) --outputdir=$(ag_regress_dir) --temp-instance=$(ag_regress_dir)/instance --port=58254 --encoding=UTF-8

ag_regress_out = instance/ log/ results/ regression.*
EXTRA_CLEAN = $(addprefix $(ag_regress_dir)/, $(ag_regress_out)) 

LTREE_SO = $(shell $(PG_CONFIG) --pkglibdir)/ltree.so
SHLIB_LINK = -Wl,--no-as-needed $(LTREE_SO) -Wl,--as-needed

ag_include_dir = $(srcdir)/src/include
PG_EXT_INCLUDE = $(shell $(PG_CONFIG) --includedir-server)/extension
PG_CPPFLAGS = -w -I$(ag_include_dir) -I$(PG_EXT_INCLUDE) -I$(PG_EXT_INCLUDE)/ltree

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

installcheck: export LC_COLLATE=C
