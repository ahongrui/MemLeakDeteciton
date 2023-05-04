all: mla test
test:
	g++ test.cpp -g -o test -lpthread -std=c++11
mla:
	g++ btrans.cpp -o btrans -std=c++11
	g++ btdiff.cpp -o btdiff -std=c++11
	g++ -fpic -shared  MemLeakDetection.cpp -g -o libmla.so -ldl -lpthread -std=c++11
clean:
	rm -f test libmla.so
