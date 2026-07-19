EXT_DIR = third_party/rpi-rgb-led-matrix-extensions
RGB_DIR = $(EXT_DIR)/third_party/rpi-rgb-led-matrix

CXXFLAGS = -Wall -O3 -I$(RGB_DIR)/include -I$(EXT_DIR)/include -I$(EXT_DIR)/third_party/json
LDFLAGS = -L$(RGB_DIR)/lib -L$(EXT_DIR) -lrgbmatrix -lledcommon -lstdc++ -lm -lpq -lpthread

COMMON_LIB = $(EXT_DIR)/libledcommon.a
TARGET = db_display
OBJS = src/db_display.o src/db_fetcher.o src/db_config.o

all: $(COMMON_LIB) $(TARGET)

.PHONY: $(COMMON_LIB)
$(COMMON_LIB):
	$(MAKE) -C $(EXT_DIR)

src/db_display.o: src/db_display.cc src/db_fetcher.h src/db_config.h
	g++ $(CXXFLAGS) -c src/db_display.cc -o src/db_display.o

src/db_fetcher.o: src/db_fetcher.cpp src/db_fetcher.h
	g++ $(CXXFLAGS) -I$(shell pg_config --includedir 2>/dev/null || echo /usr/include/postgresql) -c src/db_fetcher.cpp -o src/db_fetcher.o

src/db_config.o: src/db_config.cpp src/db_config.h
	g++ $(CXXFLAGS) -c src/db_config.cpp -o src/db_config.o

$(TARGET): $(OBJS) $(COMMON_LIB)
	g++ $(OBJS) -o $(TARGET) $(LDFLAGS) -L$(shell pg_config --libdir 2>/dev/null || echo /usr/lib)

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean
