# matlock - Matrix Lock
# See LICENCE file for copyright and licence details.


include Makevars.mk


SRC = $(wildcard lib/*.cpp)
OBJ = $(patsubst %, $(BUILD_DIR)/%, $(SRC:.cpp=.o))
DEP = $(OBJ:.o=.d)
BUILD_BIN_FILE = $(BUILD_DIR)/$(BIN_FILE)

# generated Wayland protocol glue
PROTO_HDR = $(PROTO_DIR)/ext-session-lock-v1.h
PROTO_SRC = $(PROTO_DIR)/ext-session-lock-v1.c
PROTO_OBJ = $(PROTO_DIR)/ext-session-lock-v1.o


all: $(BUILD_BIN_FILE)


# generate the protocol header and code
$(PROTO_HDR): $(LOCK_XML)
	@mkdir -p $(dir $@)
	@$(SCANNER) client-header $< $@

$(PROTO_SRC): $(LOCK_XML)
	@mkdir -p $(dir $@)
	@$(SCANNER) private-code $< $@

$(PROTO_OBJ): $(PROTO_SRC)
	@$(CC) -x c -c -O2 -o $@ $<


# compile the object files
$(BUILD_DIR)/%.o: %.cpp $(PROTO_HDR)
	@mkdir -p $(dir $@)
	@$(CC) -c $(CFLAGS) -MMD -MP -o $@ $<


# build the binary file
$(BUILD_BIN_FILE): $(OBJ) $(PROTO_OBJ)
	@$(CC) -s $(CFLAGS) -o $@ $^ $(LIBS)


# instal the build
instal: $(BUILD_BIN_FILE)
	@mkdir -p $(PREFIX)/{bin,share/{man/man1,licenses/$(BIN_FILE)}} $(SYSCONFDIR)
	@cp -f $(BUILD_BIN_FILE) $(PREFIX)/bin/
	@chmod 755 $(PREFIX)/bin/$(BIN_FILE)
	@chmod u+s $(PREFIX)/bin/$(BIN_FILE)
	@cp man/matlock.1.gz $(PREFIX)/share/man/man1
	@cp LICENCE $(PREFIX)/share/licenses/$(BIN_FILE)/
	@[ -f $(SYSCONFDIR)/matlock.yaml ] || cp etc/matlock.yaml $(SYSCONFDIR)/
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
	@cp etc/matlock.yaml $(__RELEASE_DIR)
	@tar czf $(__RELEASE_DIR).tar.gz -C $(__RELEASE_DIR)/.. $(__RELEASE_FILE)
	@rm -r $(BUILD_DIR) $(__RELEASE_DIR)
	@echo -e "Release files created and compressed.\nFile at $(__RELEASE_DIR).tar.gz"


clean:
	@rm -rf $(BUILD_DIR) $(__RELEASE_DIR).tar.gz
	@echo -e "Build files removed."


-include $(DEP)


.PHONY: all instal install uninstall build clean
