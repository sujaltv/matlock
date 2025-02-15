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
	@mkdir -p $(DESTDIR)$(PREFIX)/bin
	@cp -f $(BUILD_BIN_FILE) $(DESTDIR)$(PREFIX)/bin/
	@chmod 755 $(DESTDIR)$(PREFIX)/bin/$(BIN_FILE)
	@chmod u+s $(DESTDIR)$(PREFIX)/bin/$(BIN_FILE)
	mkdir -p $(DESTDIR)$(MAN_DIR)/man1
	@cp doc/matlock.1 $(DESTDIR)$(MAN_DIR)/man1/
	@echo -e "Program installed.\nBinary file at $(PREFIX)/bin/$(BIN_FILE)"


# alias to instal
install: instal


# uninstall
uninstall:
	@rm -rf \
		$(DESTDIR)$(PREFIX)/bin/$(BIN_FILE) \
		$(DESTDIR)$(MAN_DIR)/man1/matlock.1 \
		$(DESTDIR)$(PREFIX)/etc/$(BIN_FILE)
	@echo -e "Program uninstalled and related files deleted."


# create a release tarball
release: $(BUILD_BIN_FILE)
	@mkdir -p $(RELEASE_DIR)
	@cp $(BUILD_BIN_FILE) $(RELEASE_DIR)
	@cp doc/matlock.1 $(RELEASE_DIR)
	@cp LICENCE $(RELEASE_DIR)
	@cp README.md $(RELEASE_DIR)
	@tar czf $(RELEASE_DIR).tar.gz -C $(RELEASE_DIR)/.. $(BIN_FILE)-v$(VERSION)
	@rm -r $(RELEASE_DIR)
	@echo -e "Release files created and compressed.\nFile at $(RELEASE_DIR).tar.gz"


clean:
	@rm -rf $(BUILD_DIR)
	@echo -e "Build files removed."


.PHONY: instal uninstall release clean

