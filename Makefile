LLVM_CONFIG = llvm-config
HEADER_FILES = $(wildcard *.hpp)

CXXFLAGS = -Wall -W -Wno-unused-parameter -O2 -g -std=c++03 -fno-strict-aliasing
LDFLAGS = -g
LIBS = -lsqlite3
LLVM_CXXFLAGS := $(shell $(LLVM_CONFIG) --cxxflags) -fno-exceptions -fno-rtti
LLVM_LDFLAGS := $(shell $(LLVM_CONFIG) --ldflags)
LLVM_LIBS := $(shell $(LLVM_CONFIG) --libs support)

all: plugin.so create-db report patch-sprintf-overload

plugin.so: plugin.o util.o db-file.o db.o file.o
	g++ -shared $(LDFLAGS) -o $@ $^ $(LLVM_LDFLAGS) $(LIBS) $(LLVM_LIBS)

create-db: create-db.o db.o db-file.o util.o file.o
	g++ $(LDFLAGS) -o $@ $^ $(LLVM_LDFLAGS) $(LIBS)

report: report.o db.o db-file.o db-report.o LineEditor.o util.o file.o
	g++ $(LDFLAGS) -o $@ $^ $(LLVM_LDFLAGS) $(LIBS)

patch-sprintf-overload: patch-sprintf-overload.o db.o db-file.o db-report.o LineEditor.o util.o file.o
	g++ $(LDFLAGS) -o $@ $^ $(LLVM_LDFLAGS) $(LIBS)

%.o : %.cpp $(HEADER_FILES)
	g++ $(LLVM_CXXFLAGS) $(CXXFLAGS) -c $< -o $@
