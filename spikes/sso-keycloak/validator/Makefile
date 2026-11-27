MODULE_big = vault_validator
OBJS = vault_validator.o
SHLIB_LINK = -lcurl

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
