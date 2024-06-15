CXX = g++

SRC_FILES := $(wildcard *.cpp)
EXEC_FILES := $(patsubst %.cpp,%,$(SRC_FILES))

all: $(EXEC_FILES)

%: %.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f $(EXEC_FILES) *.log