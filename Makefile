HEADER_FILES = $(wildcard *.hpp)

CXXFLAGS = -Wall -W -O2 -g -std=c++0x
LDFLAGS = -g
LIBS = -lsqlite3
LLVM_CXXFLAGS := $(shell llvm-config --cxxflags) -fno-exceptions -fno-rtti
LLVM_LDFLAGS := $(shell llvm-config --ldflags)

all: plugin.so create-db report

plugin.so: plugin.o util.o db-file.o db.o
	g++ -shared $(LDFLAGS) -o $@ $^ $(LLVM_LDFLAGS) $(LIBS)

create-db: create-db.o db.o db-file.o util.o
	g++ $(LDFLAGS) -o $@ $^ $(LLVM_LDFLAGS) $(LIBS)

report: report.o db.o db-file.o util.o
	g++ $(LDFLAGS) -o $@ $^ $(LLVM_LDFLAGS) $(LIBS)

%.o : %.cpp $(HEADER_FILES)
	g++ $(LLVM_CXXFLAGS) $(CXXFLAGS) -c $< -o $@
