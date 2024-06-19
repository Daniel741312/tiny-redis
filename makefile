CXX = g++

SRC_FILES := $(wildcard *.cpp)
EXEC_FILES := $(patsubst %.cpp,%,$(SRC_FILES))

server: $(SRC_FILES)
	g++ $^ -o $@

clean:
	rm -f $(EXEC_FILES) *.log