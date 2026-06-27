CXX=nvcc
CXXFLAGS=-m64 -gencode=arch=compute_60,code=sm_60 -gencode=arch=compute_61,code=sm_61 -gencode=arch=compute_70,code=sm_70 -gencode=arch=compute_70,code=compute_70

vectorAdd: vectorAdd.o
	$(CXX) -O3 --cudart shared -o vectorAdd vectorAdd.o

vectorAdd.o: vecAdd.cu
	$(CXX) $(CXXFLAGS) -O3 -o vectorAdd.o -c vecAdd.cu

vectorAdd.objdump: vectorAdd
	objdump -Mno-aliases -Mnumeric --disassemble-all --disassemble-zeroes $< > $@

fft: fft.o
	$(CXX) -O3 --cudart shared -o fft fft.o

fft.o: fft.cu
	$(CXX) $(CXXFLAGS) -O3 -o fft.o -c fft.cu

fft.objdump: fft
	objdump -Mno-aliases -Mnumeric --disassemble-all --disassemble-zeroes $< > $@

# Double-precision build of the same kernels (built on demand, not in `all`).
fft_double: fft_double.o
	$(CXX) -O3 --cudart shared -o fft_double fft_double.o

fft_double.o: fft.cu
	$(CXX) $(CXXFLAGS) -DFFT_DOUBLE -O3 -o fft_double.o -c fft.cu

all: vectorAdd fft

clean:
	rm -f vectorAdd vectorAdd.o fft fft.o fft_double fft_double.o


