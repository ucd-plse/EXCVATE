all: clean main.f90 squared_hinge_losses.o
	$(FC) -g -O0 main.f90 squared_hinge_losses.o -o test

squared_hinge_losses.o: squared_hinge_losses.f90
	$(FC) -g -O0 -c squared_hinge_losses.f90

clean:
	rm -f *.mod *.o test

reset: clean
	rm -rf __EXCVATE*