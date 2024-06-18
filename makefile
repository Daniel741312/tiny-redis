CXX = g++

SRC_FILES := $(wildcard *.cpp)
EXEC_FILES := $(patsubst %.cpp,%,$(SRC_FILES))

server: server.cpp avl.cpp hashtable.cpp zset.cpp
	g++ $^ -o $@

clean:
	rm -f $(EXEC_FILES) *.log