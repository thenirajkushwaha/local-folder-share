CXX ?= g++
CXXFLAGS = -std=c++23 -Wall -Wextra -O2 -pthread
LDFLAGS = -lX11

SRCS = src/main.cpp src/file_util.cpp src/network.cpp src/cli.cpp src/gui.cpp
OBJS = $(SRCS:.cpp=.o)
TARGET = folder_share

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $(TARGET) $(CXXFLAGS) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
