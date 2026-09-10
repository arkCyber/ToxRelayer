#Install directories
PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin

LIBS = toxcore sqlite3

# Coding standard: the sources must compile warning free under -Wall -Wextra.
# -Werror turns any new warning into a build failure so it cannot be merged
# unnoticed. Set WERROR= (empty) to relax the check temporarily, e.g. when
# bringing up a new compiler or platform.
WERROR ?= -Werror

CFLAGS += -std=c11 -Wall -Wextra $(WERROR) -g -D_XOPEN_SOURCE_EXTENDED -D_XOPEN_SOURCE -D_FILE_OFFSET_BITS=64
OBJ = toxrelayer.o misc.o commands.o groupchats.o log.o msg_queue.o mq_persist.o minIni.o msg_database.o telegram.o relay.o
CFLAGS += $(shell pkg-config --cflags $(LIBS))
LDFLAGS += $(shell pkg-config --libs $(LIBS))
SRC_DIR = ./src

all: $(OBJ)
	@echo "  LD    $@"
	@$(CC) $(CFLAGS) -o toxrelayer $(OBJ) $(LDFLAGS)

# ---------------------------------------------------------------------------
# Verification targets.
#
# Each test binary links one module under test together with its suite. The
# suites are standalone: they need neither toxcore nor a network, they inject
# time instead of sleeping, and they use throw-away files under /tmp so they
# never touch production data.
# ---------------------------------------------------------------------------
TESTS_DIR  = ./tests
TEST_BINS  = test_telegram test_msg_queue test_msg_database test_reliability \
             test_misc test_log test_groupchats test_commands test_relay \
             test_mq_persist

# Modules that must be linked into each test binary (beyond the suite itself).
test_msg_database: EXTRA_SRC = $(SRC_DIR)/msg_database.c
test_msg_database: EXTRA_LIBS = $(LDFLAGS)
test_msg_queue:    EXTRA_SRC = $(SRC_DIR)/msg_queue.c
test_msg_queue:    EXTRA_LIBS =
# The durable mirror is verified against the real queue and the real file
# format; no toxcore and no database are involved.
test_mq_persist:   EXTRA_SRC = $(SRC_DIR)/msg_queue.c $(SRC_DIR)/mq_persist.c
test_mq_persist:   EXTRA_LIBS =
test_telegram:     EXTRA_SRC = $(SRC_DIR)/telegram.c
test_telegram:     EXTRA_LIBS =
test_reliability:  EXTRA_SRC = $(SRC_DIR)/telegram.c $(SRC_DIR)/msg_database.c
test_reliability:  EXTRA_LIBS = $(LDFLAGS)

# Suites for the modules that talk to toxcore; an offline Tox instance is used
# where a handle is required, so no network access happens during a test run.
test_misc:         EXTRA_SRC = $(SRC_DIR)/misc.c $(SRC_DIR)/log.c
test_misc:         EXTRA_LIBS = $(LDFLAGS)
test_log:          EXTRA_SRC = $(SRC_DIR)/log.c $(SRC_DIR)/misc.c
test_log:          EXTRA_LIBS = $(LDFLAGS)
test_groupchats:   EXTRA_SRC = $(SRC_DIR)/groupchats.c $(SRC_DIR)/misc.c $(SRC_DIR)/log.c
test_groupchats:   EXTRA_LIBS = $(LDFLAGS)
test_commands:     EXTRA_SRC = $(SRC_DIR)/commands.c $(SRC_DIR)/groupchats.c \
                              $(SRC_DIR)/misc.c $(SRC_DIR)/log.c
test_commands:     EXTRA_LIBS = $(LDFLAGS)

# The relay policy is verified against the real codec, queue and database, so
# the suite links all three alongside the module under test.
test_relay:        EXTRA_SRC = $(SRC_DIR)/relay.c $(SRC_DIR)/telegram.c \
                              $(SRC_DIR)/msg_queue.c $(SRC_DIR)/msg_database.c
test_relay:        EXTRA_LIBS = $(LDFLAGS)

TEST_CFLAGS = $(filter-out -g,$(CFLAGS))

# A suite is rebuilt when either it or a module it links changes. $< stays the
# suite source because it is listed first.
$(TEST_BINS): %: $(TESTS_DIR)/%.c $(EXTRA_SRC)
	@echo "  CC    $@"
	@$(CC) $(TEST_CFLAGS) -Isrc -o $@ $< $(EXTRA_SRC) $(EXTRA_LIBS)

# The suites are executed by this recipe rather than by the build rule, so
# `make test` always runs them. Running them from the rule meant a second
# `make test` silently did nothing, because the binaries were already up to
# date -- a verification gate that can pass without running anything.
test: $(TEST_BINS)
	@for t in $(TEST_BINS); do ./$$t || exit 1; done

# ---------------------------------------------------------------------------
# Static analysis. The enforced check set and every documented deviation live in
# .clang-tidy, which is part of the coding standard and under version control.
# ---------------------------------------------------------------------------
CLANG_TIDY ?= clang-tidy
SDKROOT    ?= $(shell xcrun --show-sdk-path 2>/dev/null)
SYSROOT_FLAG = $(if $(SDKROOT),-isysroot $(SDKROOT),)

ANALYZE_SRC = $(SRC_DIR)/toxrelayer.c $(SRC_DIR)/commands.c $(SRC_DIR)/groupchats.c \
              $(SRC_DIR)/msg_queue.c $(SRC_DIR)/mq_persist.c \
              $(SRC_DIR)/msg_database.c $(SRC_DIR)/telegram.c \
              $(SRC_DIR)/relay.c $(SRC_DIR)/misc.c $(SRC_DIR)/log.c

analyze:
	@echo "  ANALYZE clang static analyzer"
	@for f in $(ANALYZE_SRC); do \
		$(CC) -std=c11 --analyze -Xanalyzer -analyzer-output=text -Isrc \
			$(filter-out -g,$(CFLAGS)) $$f 2>&1 | grep -E 'warning:|error:' || true; \
	done
	@echo "  ANALYZE clang-tidy"
	@$(CLANG_TIDY) $(ANALYZE_SRC) --quiet -- \
		$(filter-out -g,$(CFLAGS)) $(SYSROOT_FLAG) -Isrc

# Enforcement gate. Every module is now clean, so the gate covers all of them:
# a change that introduces a finding fails the build.
CLEAN_SRC = $(ANALYZE_SRC)

analyze-strict:
	@echo "  ANALYZE (strict gate: analyzer)"
	@for f in $(CLEAN_SRC); do \
		$(CC) -std=c11 --analyze -Xanalyzer -analyzer-output=text -Isrc \
			$(filter-out -g,$(CFLAGS)) $$f || exit 1; \
	done
	@echo "  ANALYZE (strict gate: clang-tidy)"
	@$(CLANG_TIDY) $(CLEAN_SRC) --quiet -- \
		$(filter-out -g,$(CFLAGS)) $(SYSROOT_FLAG) -Isrc

# Same suites built with the address and undefined-behaviour sanitizers.
# -g is filtered out: on macOS it makes the linker invoke dsymutil, which
# cannot process a sanitizer-generated object.
test-sanitize: TEST_CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
test-sanitize: clean-sanitize test

clean-sanitize:
	@rm -f $(TEST_BINS)
	@rm -rf $(addsuffix .dSYM,$(TEST_BINS))

%.o: $(SRC_DIR)/%.c
	@echo "  CC    $@"
	@$(CC) $(CFLAGS) -o $*.o -c $(SRC_DIR)/$*.c
	@$(CC) -MM $(CFLAGS) $(SRC_DIR)/$*.c > $*.d

install: toxrelayer
	@echo "Installing toxrelayer"
	@mkdir -p $(abspath $(DESTDIR)/$(BINDIR))
	@install -m 0755 toxrelayer $(abspath $(DESTDIR)/$(BINDIR))

clean:
	rm -f *.d *.o toxrelayer $(TEST_BINS)
	rm -rf $(addsuffix .dSYM,$(TEST_BINS))

uninstall:
	@echo "Uninstalling toxrelayer"
	@rm -f $(abspath $(DESTDIR)/$(BINDIR)/toxrelayer)

.PHONY: clean all test test-sanitize clean-sanitize analyze analyze-strict
