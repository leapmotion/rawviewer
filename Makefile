CC = gcc
CXX = g++
# Update these paths to match your installation
# You may also need to update the linker option rpath, which sets where to look for
# the SDL2 libraries at runtime to match your install
SDL_LIB = -L/usr/local/lib -lSDL2 -Wl,-rpath=/usr/local/lib
SDL_INCLUDE = -I/usr/local/include

LDFLAGS = $(SDL_LIB) -lpthread

EXE = RawViewer

all: $(EXE)

$(EXE): v4l2sdl.o
	$(CC) $< $(LDFLAGS) -o $@

clean:
	rm *.o && rm $(EXE)
