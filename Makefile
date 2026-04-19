CXX = g++
CXXFLAGS = -std=c++23 -O3 -ffast-math
LDFLAGS =

release:
	$(CXX) $(CXXFLAGS) -o ifstatrec main.cpp $(LDFLAGS)

clean:
	rm -f ifstatrec
