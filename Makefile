
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

-include $(wildcard *.d)

%.png : %.grp
	dot -T png -o $@ $^
	
%.o : %.cpp $(wildcard *.h)
	g++ -c ${includes} ${cpp_flags} -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.d)" -o "$@" "$<"
	
Queue.out : $(patsubst %.cpp,%.o,${list_cpp})
	g++ ${ld_flags} -o $@ $^ ${ld_libs} ${LIBS}
	
all : $(patsubst %.grp,%.png,${list_grp}) Queue.out