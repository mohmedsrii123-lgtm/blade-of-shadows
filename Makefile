CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs) -lSDL2_image

SRC := src/main.cpp
BIN := blade

all: $(BIN)

$(BIN): $(SRC)
	$(CXX) $(CXXFLAGS) $(SDL_CFLAGS) -o $@ $(SRC) $(SDL_LIBS)

run: $(BIN)
	./$(BIN)

screenshot: $(BIN)
	./$(BIN) --screenshot screenshot-gameplay.png
	./$(BIN) --screenshot screenshot-title.png title
	./$(BIN) --screenshot screenshot-attack.png attack

clean:
	rm -f $(BIN)

.PHONY: all run screenshot clean
