
empty :=
space := $(empty) $(empty)

get_abs_path_scape = $(subst $(space),\ ,$(abspath $1))

includes = -I$(call get_abs_path_scape,../../x86/include) -includeCppUTest/MemoryLeakDetectorNewMacros.h
cpp_flags = --std=c++11 -DCPP_UTEST1 -O0 -g -Wall -fmessage-length=0
ld_flags = -L$(call get_abs_path_scape,../../x86/lib) 
#-Wl,--verbose=99

#ld_libs =  -Wl,-Bstatic -lCppUTest -Wl,-Bstatic -lCppUTestExt 
LIBS := -lpthread -lCppUTest

list_grp = $(wildcard *.grp)
list_cpp = $(wildcard *.cpp)

%.png : %.grp
	dot -T png -o $@ $^
	
%.o : %.cpp
	g++ -c ${includes} ${cpp_flags} -o $@ $^
	
Queue : $(patsubst %.cpp,%.o,${list_cpp})
	g++ ${includes} ${cpp_flags} ${ld_flags} ${ld_libs} ${LIBS} -o $@ $^
	
all : $(patsubst %.grp,%.png,${list_grp})