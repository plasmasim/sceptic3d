
# Universal Makefile for sceptic3D

#Defaults compiler (mpif77 compiler)
ifeq ("$(G77)","")
	G77=mpif77
endif
#Defaults compiler (mpif90 compiler)
ifeq ("$(G90)","")
	G90=mpif90
endif
#Default Xlib (32 bit)
ifeq ("$(XLIB)","")
	XLIB=/usr/X11R6/lib
endif
#Default Accis lib
ifeq ("$(ACCISLIB)","")
	ACCISLIB=./accis
endif

LIBRARIES =  -L$(XLIB) -L$(ACCISLIB) -laccisX -lXt -lX11 
# To figure out what to use for the hdf includes and libraries
# run the h5fc script with -show (usr/local/hdf5/bin/h5fc)
HDFINCLUDE = -I/usr/local/hdf5/include
HDFLIBRARIES = -L/usr/local/hdf5/lib -lhdf5hl_fortran -lhdf5_hl -lhdf5_fortran -lhdf5 -lz -lm -Wl,-rpath -Wl,/usr/local/hdf5/lib

#Default No Warnings
ifeq ("$(NOWARN)","")
	NOWARN=
endif

COMPILE-SWITCHES =-Wall -Wno-unused-variable  $(NOWARN)  -O2  -I.
# For debugging.
#  -g  -ffortran-bounds-check
# For profiling
#COMPILE-SWITCHES = -Wall -O2 -pg

REINJECT=orbitinject.o extint.o maxreinject.o mcreinject.o

MPICOMPILE-SWITCHES = -DMPI $(COMPILE-SWITCHES)

OBJECTS = initiate.o advancing.o randc.o randf.o diags.o outputs.o outputhdf.o	\
 chargefield.o $(REINJECT) stringsnames.o		\
rhoinfcalc.o shielding3D.o utils.o


MPIOBJECTS=cg3dmpi.o mpibbdy.o shielding3D_par.o

all : makefile sceptic3D

sceptic3D :  makefile sceptic3D.F  piccom.f  ./accis/libaccisX.a $(OBJECTS)
	$(G77) $(COMPILE-SWITCHES) $(HDFINCLUDE) $(HDFLIBRARIES) -o sceptic3D sceptic3D.F  $(OBJECTS) $(LIBRARIES)

# The real Makefile
MAKEFILE=makefile

makefile : Makefile MFSconfigure
	@echo Configuring the Makefile for this platform.
	rm -f ./accis/makefile
	rm -f *.o
	rm -f *./accis/*.o
	make -C accis
	./MFSconfigure
	@echo Now running make again using the new Makefile
	make -f $(MAKEFILE)

sceptic3Dmpi : sceptic3D.F  piccom.f piccomcg.f ./accis/libaccisX.a $(OBJECTS) $(MPIOBJECTS) makefile
	$(G77) $(MPICOMPILE-SWITCHES) $(HDFINCLUDE) $(HDFLIBRARIES) -o sceptic3Dmpi  sceptic3D.F   $(OBJECTS) $(MPIOBJECTS) $(LIBRARIES)

./accis/libaccisX.a : ./accis/*.f
	make -C accis

orbitint : orbitint.f coulflux.o $(OBJECTS) ./accis/libaccisX.a makefile
	$(G77) $(COMPILE-SWITCHES) -o orbitint orbitint.f $(OBJECTS) coulflux.o $(LIBRARIES)

coulflux.o : tools/coulflux.f
	$(G77) -c $(COMPILE-SWITCHES) tools/coulflux.f

fvinjecttest : fvinjecttest.F makefile fvinject.o reinject.o initiate.o advancing.o chargefield.o randf.o fvcom.f
	$(G77)  -o fvinjecttest $(COMPILE-SWITCHES) fvinjecttest.F fvinject.o reinject.o initiate.o advancing.o chargefield.o randf.o  $(LIBRARIES)

fvinject.o : fvinject.f fvcom.f piccom.f
	$(G77) -c $(COMPILE-SWITCHES) fvinject.f

outputhdf.o : outputhdf.f piccom.f colncom.f
	$(G90) -c $(COMPILE-SWITCHES)  $(HDFINCLUDE) outputhdf.f

#pattern rule
%.o : %.f piccom.f fvcom.f makefile;
	$(G77) -c $(COMPILE-SWITCHES) $*.f

%.o : %.F piccom.f makefile;
	$(G77) -c $(COMPILE-SWITCHES) $*.F

% : %.f makefile
	$(G77)  -o $* $(COMPILE-SWITCHES) $*.f  $(LIBRARIES)

% : %.F makefile
	$(G77)  -o $* $(COMPILE-SWITCHES) $*.F  $(LIBRARIES)

sceptic3D.tar.gz : ./accis/libaccisX.a sceptic3D sceptic3Dmpi
	make -C accis mproper
	make -C tools clean
	make clean
	./copyattach.sh
	tar chzf sceptic3D.tar.gz -C .. sceptic3D
	./copyremove.sh

clean :
	rm -f *.o
	rm -f *.ps
	rm -f *.orb
	rm -f *.html
	rm -f Orbits.txt
	rm -f *~
	rm -f makefile

cleanall :
	make clean
	rm -f *.dat
	rm -f *.frc

ftnchek :
	ftnchek -nocheck -nof77 -calltree=text,no-sort -mkhtml -quiet -brief sceptic3D.F *.f

