PROJECT := copilot-sessions
VERSION ?= 0.1.0
BUILD_DIR ?= build
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

CXX ?= g++
PKG_CONFIG ?= pkg-config
PKGS := notcurses sqlite3
BUILD ?= release

SRC := copilot-sessions.cpp
OBJ := $(BUILD_DIR)/copilot-sessions.o
DEP := $(BUILD_DIR)/copilot-sessions.d

STD := -std=c++20
WARNFLAGS := -Wall -Wextra -Wpedantic
DEPFLAGS := -MMD -MP

ifeq ($(shell $(PKG_CONFIG) --exists $(PKGS) && printf yes),)
$(error Required pkg-config packages not found: $(PKGS))
endif

ifeq ($(BUILD),debug)
OPTFLAGS := -O0 -g3
CPPFLAGS += -DDEBUG
else ifeq ($(BUILD),release)
OPTFLAGS := -O2 -g -DNDEBUG
else
$(error Unsupported BUILD '$(BUILD)'; use BUILD=debug or BUILD=release)
endif

CPPFLAGS += $(shell $(PKG_CONFIG) --cflags $(PKGS))
CPPFLAGS += -DPROJECT_VERSION=\"$(VERSION)\"
CXXFLAGS += $(STD) $(WARNFLAGS) $(OPTFLAGS)
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PKGS))

.PHONY: all clean run debug release install uninstall

all: $(PROJECT)

$(PROJECT): $(OBJ)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(OBJ): $(SRC)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

run: $(PROJECT)
	./$(PROJECT)

debug:
	$(MAKE) BUILD=debug all

release:
	$(MAKE) BUILD=release all

install: $(PROJECT)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(PROJECT) $(DESTDIR)$(BINDIR)/$(PROJECT)

uninstall:
	$(RM) $(DESTDIR)$(BINDIR)/$(PROJECT)

clean:
	$(RM) $(PROJECT)
	$(RM) -r $(BUILD_DIR)

-include $(DEP)
