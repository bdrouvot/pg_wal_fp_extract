PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
PGVERSION := $(shell $(PG_CONFIG) --version)
include $(PGXS)

ifeq ($(findstring 13.,$(PGVERSION)),13.)
    OBJS = pg_wal_fp_extract.o pg_xlogreader_13.o
endif

ifeq ($(findstring 12.,$(PGVERSION)),12.)
    OBJS = pg_wal_fp_extract.o pg_xlogreader_12.o
endif

ifeq ($(findstring 11.,$(PGVERSION)),11.)
    OBJS = pg_wal_fp_extract.o pg_xlogreader_11.o
endif

ifeq ($(findstring 10.,$(PGVERSION)),10.)
    OBJS = pg_wal_fp_extract.o pg_xlogreader_10.o
endif

override CPPFLAGS := -DFRONTEND $(CPPFLAGS)

all: pg_wal_fp_extract

pg_wal_fp_extract: $(OBJS)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)
