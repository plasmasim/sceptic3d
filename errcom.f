c*********************************************************************
c Error handling and debugging
c     Flag to go to output
      logical lgotooutput
c     Flag indicating whether to save potential for nstepsssave steps
      logical lsavephi
c     Max grid size and steps to save
      integer nrsizesave,nthsizesave,npsisizesave,nstepssave
      parameter (nrsizesave=61,nthsizesave=11,npsisizesave=11
     $  ,nstepssave=2001)
c     The potential normalized to Te/e
      real phisave(0:nrsizesave,0:nthsizesave,0:npsisizesave,
     $  nstepssave)
c     The potential on axis (cos(theta)=+-1) before averaging
      real phiaxissave(0:nrsizesave,2,0:npsisizesave,nstepssave)
c     The matrix A and its transpose at some step
      real Asave(0:nrsizesave,0:nthsizesave,0:npsisizesave,
     $  0:nrsizesave,0:nthsizesave,0:npsisizesave)
      real Atsave(nrsizesave,nthsizesave,npsisizesave,
     $  nrsizesave,nthsizesave,npsisizesave)
c     The matrix A and its transpose at some step
      real Amat(nrsizesave*nthsizesave*npsisizesave,
     $  nrsizesave*nthsizesave*npsisizesave)
      real Atmat(nrsizesave*nthsizesave*npsisizesave,
     $  nrsizesave*nthsizesave*npsisizesave)
c     Flag to indicate that something hasn't been done
      logical lfirsttime
c     Counter to do something at a certain step
      integer stepcount
c     Input and output vectors to cg3D
      real bsave(nrsizesave-1,0:nthsizesave,0:npsisizesave)
     $     ,xsave(nrsizesave-1,0:nthsizesave,0:npsisizesave)
c     Input and output vectors to cg3D
      real bsavevect((nrsizesave-1)*(nthsizesave+1)*(npsisizesave+1))
     $     ,xsavevect((nrsizesave-1)*(nthsizesave+1)*(npsisizesave+1))
c     Outermost r cell to consider
      integer rshieldingsave
c     Flag indicating whether to save matrices from Poisson calculation
      logical lsavemat
c     Step to save at
      integer saveatstep
c     Flag signaling for solver to only multiply by A (not iterate)
      logical lAdebug
c     Dimensional structure for Asave (see piccomcg.f)
c     Not included in common block for now, but don't really need to be
c       since only used in one function
      integer ndimssave
      parameter (ndimssave=3)
      integer iLssave(ndimssave+1)
      integer iudssave(ndimssave)
      integer idimssave(ndimssave)
      integer icoordssave(ndimssave)
      integer iLcoordssave(ndimssave+1)
      integer norigmaxsave
      parameter (norigmaxsave=1000)
      integer iorigsave(norigmaxsave)
      integer mysidesave(ndimssave)
      integer myorigsave,myorig1save,myorig2save,myorig3save
      integer ifullsave(ndimssave)
      logical outsave,innsave
      integer mycartidsave
      integer icommcartsave

c     Error handling common block
      common /err/lgotooutput,lsavephi,phisave,phiaxissave,Asave,Atsave,
     $  lfirsttime,bsave,xsave,rshieldingsave,stepcount,Amat,Atmat,
     $  bsavevect,xsavevect,lsavemat,saveatstep,lAdebug
