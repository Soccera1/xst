# xst - A simple X11+OpenGL terminal
# Makefile

# --- Configuration ---
# These variables are easily modifiable.

# Compiler and primary flags
# Use 'cc' for portability, which usually links to gcc.
CC = cc

# CFLAGS:
# -std=c99:       Enforce the C99 standard.
# -pedantic:      Issue all warnings demanded by the standard; reject non-standard extensions.
# -Wall -Wextra:  Enable all major warnings plus extra ones.
# -O3:            Optimize for performance.
# pkg-config:     Automatically find required headers for external libraries.
# -flto           Link time optimization.
# -march=native   Compile for native CPU.
CFLAGS   = -std=c99 -pedantic -Wall -Wextra -O3 -flto -march=native $(shell pkg-config --cflags x11 gl freetype2)

# LDFLAGS:
# pkg-config: Finds the required library flags for X11, GL, and FreeType.
# -lutil:     Links against the utility library for forkpty().
# -lm:        Links against the math library.
LDFLAGS  = $(shell pkg-config --libs x11 gl freetype2) -lutil -lm

# Installation directories
# PREFIX is the base directory for installation (e.g., /usr/local or /usr).
PREFIX   = /usr/local
# BINDIR is where the executable will be installed.
BINDIR   = $(PREFIX)/bin
# APPDIR is the standard directory for .desktop files.
APPDIR   = /usr/share/applications

# --- Files ---
# Source, object, and target executable names.
SRC      = src/xst.c
OBJ      = src/xst.o
TARGET   = xst


# --- Rules ---

# Default rule: 'make' or 'make all' will build the executable.
.PHONY: all
all: $(TARGET)

# Link the object file into the final executable.
$(TARGET): $(OBJ)
	@echo "LD   $(TARGET)"
	@$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS)

# Compile the source code into an object file.
$(OBJ): $(SRC)
	@echo "CC   $(SRC)"
	@$(CC) $(CFLAGS) -c $(SRC) -o $(OBJ)

# Clean up build files.
.PHONY: clean
clean:
	@echo "CLEAN"
	@rm -f $(TARGET) $(OBJ)

# Install the executable and .desktop file system-wide.
# Must be run with 'sudo make install'.
.PHONY: install
install: all
	@echo "Installing $(TARGET) to $(BINDIR)..."
	@mkdir -p "$(DESTDIR)$(BINDIR)"
	@cp -f "$(TARGET)" "$(DESTDIR)$(BINDIR)"
	@chmod 755 "$(DESTDIR)$(BINDIR)/$(TARGET)"

	@echo "Installing desktop file to $(APPDIR)..."
	@mkdir -p "$(DESTDIR)$(APPDIR)"
	@cp -f xst.desktop "$(DESTDIR)$(APPDIR)"

	@echo "Updating desktop database..."
	@update-desktop-database -q "$(DESTDIR)$(APPDIR)"

	@echo "Installation complete."
	@echo "Run 'xst' or find it in your application menu."

# Uninstall the executable and .desktop file.
# Must be run with 'sudo make uninstall'.
.PHONY: uninstall
uninstall:
	@echo "Uninstalling $(TARGET) from $(BINDIR)..."
	@rm -f "$(DESTDIR)$(BINDIR)/$(TARGET)"

	@echo "Uninstalling desktop file from $(APPDIR)..."
	@rm -f "$(DESTDIR)$(APPDIR)/xst.desktop"

	@echo "Updating desktop database..."
	@update-desktop-database -q "$(DESTDIR)$(APPDIR)"
