# matlock - Matrix Lock
# See LICENCE file for copyright and licence details.


include Makevars.mk


SRC = $(wildcard lib/*.cpp)
OBJ = $(patsubst %, $(BUILD_DIR)/%, $(SRC:.cpp=.o))
BUILD_BIN_FILE = $(BUILD_DIR)/$(BIN_FILE)


# compile the object files
$(OBJ): $(SRC)
	@mkdir -p $(dir $@)
	@$(CC) -c $(CFLAGS) -o $@ $(LIBS) $(patsubst $(BUILD_DIR)/%.o, %.cpp, $@)


# build the binary file
$(BUILD_BIN_FILE): $(OBJ)
	@$(CC) -s $(LIBS) -o $@ $^


# instal the build
instal: $(BUILD_BIN_FILE)
	@mkdir -p $(PREFIX)/{bin,share/{man/man1,licenses/$(BIN_FILE)}}
	@cp -f $(BUILD_BIN_FILE) $(PREFIX)/bin/
	@chmod 755 $(PREFIX)/bin/$(BIN_FILE)
	@chmod u+s $(PREFIX)/bin/$(BIN_FILE)
	@cp man/matlock.1.gz $(PREFIX)/share/man/man1
	@cp LICENCE $(PREFIX)/share/licenses/$(BIN_FILE)/
	@echo -e "Program installed.\nBinary file at $(PREFIX)/bin/$(BIN_FILE)"


# alias to instal
install: instal


# uninstall
uninstall:
	@rm -rf \
		$(PREFIX)/bin/$(BIN_FILE) \
		$(PREFIX)/share/man/man1/matlock.1.gz \
		$(PREFIX)/share/licenses/$(BIN_FILE)
	@echo -e "Program uninstalled and related files deleted."


# create a release tarball
build: $(BUILD_BIN_FILE)
	@mkdir -p $(__RELEASE_DIR)
	@cp $(BUILD_BIN_FILE) $(__RELEASE_DIR)
	@cp man/matlock.1.gz $(__RELEASE_DIR)
	@cp LICENCE $(__RELEASE_DIR)
	@cp README.md $(__RELEASE_DIR)
	@tar czf $(__RELEASE_DIR).tar.gz -C $(__RELEASE_DIR)/.. $(__RELEASE_FILE)
	@rm -r $(BUILD_DIR) $(__RELEASE_DIR)
	@echo -e "Release files created and compressed.\nFile at $(__RELEASE_DIR).tar.gz"


clean:
	@rm -rf $(BUILD_DIR) $(__RELEASE_DIR).tar.gz
	@echo -e "Build files removed."


.PHONY: instal uninstall release clean

