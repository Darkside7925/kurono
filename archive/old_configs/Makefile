CC = gcc
CFLAGS = -Wall -Wextra -O2 -I.
LDFLAGS = -lcrypto -lssl

TARGET = kurono_os
BUILD_DIR = Build_Files
SOURCE_DIR = .

SOURCES = $(wildcard $(SOURCE_DIR)/*.c)
OBJECTS = $(patsubst $(SOURCE_DIR)/%.c,$(BUILD_DIR)/%.o,$(SOURCES))
HEADERS = $(wildcard $(SOURCE_DIR)/*.h)

.PHONY: all clean install test

all: $(BUILD_DIR) $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)
	@echo "Build complete: $(TARGET)"

$(BUILD_DIR)/%.o: $(SOURCE_DIR)/%.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)
	rm -f $(TARGET)

install: $(TARGET)
	@echo "Installing Kurono OS to D:\Kurono\KuronoOS\Build_Files"
	mkdir -p "D:\Kurono\KuronoOS\Build_Files"
	cp $(TARGET) "D:\Kurono\KuronoOS\Build_Files\"
	cp *.h "D:\Kurono\KuronoOS\Build_Files\"
	@echo "Installation complete"

test: $(TARGET)
	@echo "Running Kurono OS tests..."
	./$(TARGET) --test

# Copy source to D:\Important
backup:
	@echo "Copying source to D:\Important"
	mkdir -p "D:\Important\Kurono"
	cp -r *.c *.h "D:\Important\Kurono\"
	@echo "Source backup complete"