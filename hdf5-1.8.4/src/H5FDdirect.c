/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.                                               *
 * Copyright by the Board of Trustees of the University of Illinois.         *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of HDF5.  The full HDF5 copyright notice, including     *
 * terms governing use, modification, and redistribution, is contained in    *
 * the files COPYING and Copyright.html.  COPYING can be found at the root   *
 * of the source code distribution tree; Copyright.html can be found at the  *
 * root level of an installed copy of the electronic HDF5 document set and   *
 * is linked from the top-level documents page.  It can also be found at     *
 * http://hdfgroup.org/HDF5/doc/Copyright.html.  If you do not have          *
 * access to either file, you may request a copy from help@hdfgroup.org.     *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Programmer:  Raymond Lu <slu@hdfgroup.uiuc.edu>
 *              Wednesday, 20 September 2006
 *
 * Purpose:	The Direct I/O file driver forces the data to be written to
 *		the file directly without being copied into system kernel
 *		buffer.  The main system support this feature is Linux.
 */

/* Interface initialization */
#define H5_INTERFACE_INIT_FUNC	H5FD_direct_init_interface

/* For system function posix_memalign - Commented it out because copper isn't able to compile
 * this file. */
/* #define _XOPEN_SOURCE 600 */

#include "H5private.h"		/* Generic Functions			*/
#include "H5Eprivate.h"		/* Error handling		  	*/
#include "H5Fprivate.h"		/* File access				*/
#include "H5FDprivate.h"	/* File drivers				*/
#include "H5FDdirect.h"		/* Direct file driver			*/
#include "H5FLprivate.h"	/* Free Lists                           */
#include "H5Iprivate.h"		/* IDs			  		*/
#include "H5MMprivate.h"	/* Memory management			*/
#include "H5Pprivate.h"		/* Property lists			*/

#ifdef H5_HAVE_DIRECT

/* The driver identification number, initialized at runtime */
static hid_t H5FD_DIRECT_g = 0;

/* File operations */
#define OP_UNKNOWN	0
#define OP_READ		1
#define OP_WRITE	2

/* Driver-specific file access properties */
typedef struct H5FD_direct_fapl_t {
    size_t	mboundary;	/* Memory boundary for alignment		*/
    size_t	fbsize;		/* File system block size			*/
    size_t	cbsize;		/* Maximal buffer size for copying user data	*/
    hbool_t     must_align;     /* Decides if data alignment is required        */
} H5FD_direct_fapl_t;

/*
 * The description of a file belonging to this driver. The `eoa' and `eof'
 * determine the amount of hdf5 address space in use and the high-water mark
 * of the file (the current size of the underlying Unix file). The `pos'
 * value is used to eliminate file position updates when they would be a
 * no-op. Unfortunately we've found systems that use separate file position
 * indicators for reading and writing so the lseek can only be eliminated if
 * the current operation is the same as the previous operation.  When opening
 * a file the `eof' will be set to the current file size, `eoa' will be set
 * to zero, `pos' will be set to H5F_ADDR_UNDEF (as it is when an error
 * occurs), and `op' will be set to H5F_OP_UNKNOWN.
 */
typedef struct H5FD_direct_t {
    H5FD_t	pub;			/*public stuff, must be first	*/
    int		fd;			/*the unix file			*/
    haddr_t	eoa;			/*end of allocated region	*/
    haddr_t	eof;			/*end of file; current file size*/
    haddr_t	pos;			/*current file I/O position	*/
    int		op;			/*last operation		*/
    H5FD_direct_fapl_t	fa;		/*file access properties	*/
#ifndef _WIN32
    /*
     * On most systems the combination of device and i-node number uniquely
     * identify a file.
     */
    dev_t	device;			/*file device number		*/
#ifdef H5_VMS
    ino_t	inode[3];		/*file i-node number		*/
#else
    ino_t	inode;			/*file i-node number		*/
#endif /*H5_VMS*/
#else
    /*
     * On _WIN32 the low-order word of a unique identifier associated with the
     * file and the volume serial number uniquely identify a file. This number
     * (which, both? -rpm) may change when the system is restarted or when the
     * file is opened. After a process opens a file, the identifier is
     * constant until the file is closed. An application can use this
     * identifier and the volume serial number to determine whether two
     * handles refer to the same file.
     */
    DWORD fileindexlo;
    DWORD fileindexhi;
#endif
} H5FD_direct_t;

/*
 * This driver supports systems that have the lseek64() function by defining
 * some macros here so we don't have to have conditional compilations later
 * throughout the code.
 *
 * file_offset_t:	The datatype for file offsets, the second argument of
 *			the lseek() or lseek64() call.
 *
 * file_seek:		The function which adjusts the current file position,
 *			either lseek() or lseek64().
 */
/* adding for windows NT file system support. */

#ifdef H5_HAVE_LSEEK64
#   define file_offset_t	off64_t
#   define file_seek		lseek64
#   define file_truncate	ftruncate64
#elif defined (_WIN32) && !defined(__MWERKS__)
# /*MSVC*/
#   define file_offset_t __int64
#   define file_seek _lseeki64
#   define file_truncate	_chsize
#else
#   define file_offset_t	off_t
#   define file_seek		lseek
#   define file_truncate	HDftruncate
#endif

/*
 * These macros check for overflow of various quantities.  These macros
 * assume that file_offset_t is signed and haddr_t and size_t are unsigned.
 *
 * ADDR_OVERFLOW:	Checks whether a file address of type `haddr_t'
 *			is too large to be represented by the second argument
 *			of the file seek function.
 *
 * SIZE_OVERFLOW:	Checks whether a buffer size of type `hsize_t' is too
 *			large to be represented by the `size_t' type.
 *
 * REGION_OVERFLOW:	Checks whether an address and size pair describe data
 *			which can be addressed entirely by the second
 *			argument of the file seek function.
 */
#define MAXADDR (((haddr_t)1<<(8*sizeof(file_offset_t)-1))-1)
#define ADDR_OVERFLOW(A)	(HADDR_UNDEF==(A) ||			      \
				 ((A) & ~(haddr_t)MAXADDR))
#define SIZE_OVERFLOW(Z)	((Z) & ~(hsize_t)MAXADDR)
#define REGION_OVERFLOW(A,Z)	(ADDR_OVERFLOW(A) || SIZE_OVERFLOW(Z) ||      \
                                 HADDR_UNDEF==(A)+(Z) ||		      \
				 (file_offset_t)((A)+(Z))<(file_offset_t)(A))

/* Prototypes */
static void *H5FD_direct_fapl_get(H5FD_t *file);
static void *H5FD_direct_fapl_copy(const void *_old_fa);
static H5FD_t *H5FD_direct_open(const char *name, unsigned flags, hid_t fapl_id,
			      haddr_t maxaddr);
static herr_t H5FD_direct_close(H5FD_t *_file);
static int H5FD_direct_cmp(const H5FD_t *_f1, const H5FD_t *_f2);
static herr_t H5FD_direct_query(const H5FD_t *_f1, unsigned long *flags);
static haddr_t H5FD_direct_get_eoa(const H5FD_t *_file, H5FD_mem_t type);
static herr_t H5FD_direct_set_eoa(H5FD_t *_file, H5FD_mem_t type, haddr_t addr);
static haddr_t H5FD_direct_get_eof(const H5FD_t *_file);
static herr_t  H5FD_direct_get_handle(H5FD_t *_file, hid_t fapl, void** file_handle);
static herr_t H5FD_direct_read(H5FD_t *_file, H5FD_mem_t type, hid_t fapl_id, haddr_t addr,
			     size_t size, void *buf);
static herr_t H5FD_direct_write(H5FD_t *_file, H5FD_mem_t type, hid_t fapl_id, haddr_t addr,
			      size_t size, const void *buf);
static herr_t H5FD_direct_truncate(H5FD_t *_file, hid_t dxpl_id, hbool_t closing);

static const H5FD_class_t H5FD_direct_g = {
    "direct",					/*name			*/
    MAXADDR,					/*maxaddr		*/
    H5F_CLOSE_WEAK,				/* fc_degree		*/
    NULL,					/*sb_size		*/
    NULL,					/*sb_encode		*/
    NULL,					/*sb_decode		*/
    sizeof(H5FD_direct_fapl_t),                 /*fapl_size		*/
    H5FD_direct_fapl_get,		        /*fapl_get		*/
    H5FD_direct_fapl_copy,		        /*fapl_copy		*/
    NULL,		        		/*fapl_free		*/
    0,						/*dxpl_size		*/
    NULL,					/*dxpl_copy		*/
    NULL,					/*dxpl_free		*/
    H5FD_direct_open,			        /*open			*/
    H5FD_direct_close,		                /*close			*/
    H5FD_direct_cmp,			        /*cmp			*/
    H5FD_direct_query,		                /*query			*/
    NULL,					/*get_type_map		*/
    NULL,					/*alloc			*/
    NULL,					/*free			*/
    H5FD_direct_get_eoa,			/*get_eoa		*/
    H5FD_direct_set_eoa, 			/*set_eoa		*/
    H5FD_direct_get_eof,			/*get_eof		*/
    H5FD_direct_get_handle,                     /*get_handle            */
    H5FD_direct_read,				/*read			*/
    H5FD_direct_write,				/*write			*/
    NULL,					/*flush			*/
    H5FD_direct_truncate,			/*truncate		*/
    NULL,                                       /*lock                  */
    NULL,                                       /*unlock                */
    H5FD_FLMAP_SINGLE 				/*fl_map		*/
};

/* Declare a free list to manage the H5FD_direct_t struct */
H5FL_DEFINE_STATIC(H5FD_direct_t);


/*--------------------------------------------------------------------------
NAME
   H5FD_direct_init_interface -- Initialize interface-specific information
USAGE
    herr_t H5FD_direct_init_interface()

RETURNS
    Non-negative on success/Negative on failure
DESCRIPTION
    Initializes any interface-specific data or routines.  (Just calls
    H5FD_direct_init currently).

--------------------------------------------------------------------------*/
static herr_t
H5FD_direct_init_interface(void)
{
    FUNC_ENTER_NOAPI_NOINIT_NOFUNC(H5FD_direct_init_interface)

    FUNC_LEAVE_NOAPI(H5FD_direct_init())
} /* H5FD_direct_init_interface() */


/*-------------------------------------------------------------------------
 * Function:	H5FD_direct_init
 *
 * Purpose:	Initialize this driver by registering the driver with the
 *		library.
 *
 * Return:	Success:	The driver ID for the direct driver.
 *
 *		Failure:	Negative.
 *
 * Programmer:	Raymond Lu
 *              Wednesday, 20 September 2006
 *
 * Modifications:
 *
 *-------------------------------------------------------------------------
 */
hid_t
H5FD_direct_init(void)
{
    hid_t ret_value;        /* Return value */

    FUNC_ENTER_NOAPI(H5FD_direct_init, FAIL)

    if (H5I_VFL!=H5I_get_type(H5FD_DIRECT_g))
        H5FD_DIRECT_g = H5FD_register(&H5FD_direct_g,sizeof(H5FD_class_t),FALSE);

    /* Set return value */
    ret_value=H5FD_DIRECT_g;

done:
    FUNC_LEAVE_NOAPI(ret_value)
}


/*---------------------------------------------------------------------------
 * Function:	H5FD_direct_term
 *
 * Purpose:	Shut down the VFD
 *
 * Return:	<none>
 *
 * Programmer:  Raymond Lu
 *              Wednesday, 20 September 2006
 *
 * Modification:
 *
 *---------------------------------------------------------------------------
 */
void
H5FD_direct_term(void)
{
    FUNC_ENTER_NOAPI_NOINIT_NOFUNC(H5FD_direct_term)

    /* Reset VFL ID */
    H5FD_DIRECT_g=0;

    FUNC_LEAVE_NOAPI_VOID
} /* end H5FD_direct_term() */


/*-------------------------------------------------------------------------
 * Function:	H5Pset_fapl_direct
 *
 * Purpose:	Modify the file access property list to use the H5FD_DIRECT
 *		driver defined in this source file.  There are no driver
 *		specific properties.
 *
 * Return:	Non-negative on success/Negative on failure
 *
 * Programmer:	Raymond Lu
 *		Wednesday, 20 September 2006
 *
 * Modifications:
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5Pset_fapl_direct(hid_t fapl_id, size_t boundary, size_t block_size, size_t cbuf_size)
{
    H5P_genplist_t    	*plist;      /* Property list pointer */
    H5FD_direct_fapl_t	fa;
    herr_t 		ret_value;

    FUNC_ENTER_API(H5Pset_fapl_direct, FAIL)
    H5TRACE4("e", "izzz", fapl_id, boundary, block_size, cbuf_size);

    if(NULL == (plist = H5P_object_verify(fapl_id,H5P_FILE_ACCESS)))
        HGOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "not a file access property list")

    if(boundary != 0)
    	fa.mboundary = boundary;
    else
	fa.mboundary = MBOUNDARY_DEF;
    if(block_size != 0)
    	fa.fbsize = block_size;
    else
	fa.fbsize = FBSIZE_DEF;
    if(cbuf_size != 0)
    	fa.cbsize = cbuf_size;
    else
	fa.cbsize = CBSIZE_DEF;

    /* Set the default to be true for data alignment */
    fa.must_align = TRUE;

    /* Copy buffer size must be a multiple of file block size */
    if(fa.cbsize % fa.fbsize != 0)
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "copy buffer size must be a multiple of block size")

    ret_value= H5P_set_driver(plist, H5FD_DIRECT, &fa);

done:
    FUNC_LEAVE_API(ret_value)
}


/*-------------------------------------------------------------------------
 * Function:	H5Pget_fapl_direct
 *
 * Purpose:	Returns information about the direct file access property
 *		list though the function arguments.
 *
 * Return:	Success:	Non-negative
 *
 *		Failure:	Negative
 *
 * Programmer:	Raymond Lu
 *              Wednesday, October 18, 2006
 *
 * Modifications:
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5Pget_fapl_direct(hid_t fapl_id, size_t *boundary/*out*/, size_t *block_size/*out*/,
		size_t *cbuf_size/*out*/)
{
    H5FD_direct_fapl_t	*fa;
    H5P_genplist_t *plist;      /* Property list pointer */
    herr_t      ret_value=SUCCEED;       /* Return value */

    FUNC_ENTER_API(H5Pget_fapl_direct, FAIL)
    H5TRACE4("e", "ixxx", fapl_id, boundary, block_size, cbuf_size);

    if(NULL == (plist = H5P_object_verify(fapl_id,H5P_FILE_ACCESS)))
        HGOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "not a file access list")
    if (H5FD_DIRECT!=H5P_get_driver(plist))
        HGOTO_ERROR(H5E_PLIST, H5E_BADVALUE, FAIL, "incorrect VFL driver")
    if (NULL==(fa=H5P_get_driver_info(plist)))
        HGOTO_ERROR(H5E_PLIST, H5E_BADVALUE, FAIL, "bad VFL driver info")
    if (boundary)
        *boundary = fa->mboundary;
    if (block_size)
	*block_size = fa->fbsize;
    if (cbuf_size)
	*cbuf_size = fa->cbsize;

done:
    FUNC_LEAVE_API(ret_value)
}


/*-------------------------------------------------------------------------
 * Function:	H5FD_direct_fapl_get
 *
 * Purpose:	Returns a file access property list which indicates how the
 *		specified file is being accessed. The return list could be
 *		used to access another file the same way.
 *
 * Return:	Success:	Ptr to new file access property list with all
 *				members copied from the file struct.
 *
 *		Failure:	NULL
 *
 * Programmer:	Raymond Lu
 *              Wednesday, 18 October 2006
 *
 * Modifications:
 *
 *-------------------------------------------------------------------------
 */
static void *
H5FD_direct_fapl_get(H5FD_t *_file)
{
    H5FD_direct_t	*file = (H5FD_direct_t*)_file;
    void *ret_value;    /* Return value */

    FUNC_ENTER_NOAPI(H5FD_direct_fapl_get, NULL)

    /* Set return value */
    ret_value= H5FD_direct_fapl_copy(&(file->fa));

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5FD_direct_fapl_get() */


/*-------------------------------------------------------------------------
 * Function:	H5FD_direct_fapl_copy
 *
 * Purpose:	Copies the direct-specific file access properties.
 *
 * Return:	Success:	Ptr to a new property list
 *
 *		Failure:	NULL
 *
 * Programmer:	Raymond Lu
 *              Wednesday, 18 October 2006
 *
 * Modifications:
 *
 *-------------------------------------------------------------------------
 */
static void *
H5FD_direct_fapl_copy(const void *_old_fa)
{
    const H5FD_direct_fapl_t *old_fa = (const H5FD_direct_fapl_t*)_old_fa;
    H5FD_direct_fapl_t *new_fa = H5MM_malloc(sizeof(H5FD_direct_fapl_t));
    void *ret_value;    /* Return value */

    FUNC_ENTER_NOAPI(H5FD_direct_fapl_copy, NULL)

    assert(new_fa);

    /* Copy the general information */
    HDmemcpy(new_fa, old_fa, sizeof(H5FD_direct_fapl_t));

    /* Set return value */
    ret_value=new_fa;

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5FD_direct_fapl_copy() */


/*-------------------------------------------------------------------------
 * Function:	H5FD_direct_open
 *
 * Purpose:	Create and/or opens a Unix file for direct I/O as an HDF5 file.
 *
 * Return:	Success:	A pointer to a new file data structure. The
 *				public fields will be initialized by the
 *				caller, which is always H5FD_open().
 *
 *		Failure:	NULL
 *
 * Programmer:	Raymond Lu
 *              Wednesday, 20 September 2006
 *
 * Modifications:
 *
 *-------------------------------------------------------------------------
 */
static H5FD_t *
H5FD_direct_open(const char *name, unsigned flags, hid_t fapl_id, haddr_t maxaddr)
{
    int			o_flags;
    int			fd=(-1);
    H5FD_direct_t	*file=NULL;
    H5FD_direct_fapl_t	*fa;
#ifdef _WIN32
    HFILE 		filehandle;
    struct _BY_HANDLE_FILE_INFORMATION fileinfo;
#endif
    h5_stat_t		sb;
    H5P_genplist_t 	*plist;      /* Property list */
    int                 *buf1, *buf2;
    H5FD_t		*ret_value;

    FUNC_ENTER_NOAPI(H5FD_direct_open, NULL)

    /* Sanity check on file offsets */
    assert(sizeof(file_offset_t)>=sizeof(size_t));

    /* Check arguments */
    if (!name || !*name)
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, NULL, "invalid file name")
    if (0==maxaddr || HADDR_UNDEF==maxaddr)
        HGOTO_ERROR(H5E_ARGS, H5E_BADRANGE, NULL, "bogus maxaddr")
    if (ADDR_OVERFLOW(maxaddr))
        HGOTO_ERROR(H5E_ARGS, H5E_OVERFLOW, NULL, "bogus maxaddr")

    /* Build the open flags */
    o_flags = (H5F_ACC_RDWR & flags) ? O_RDWR : O_RDONLY;
    if (H5F_ACC_TRUNC & flags) o_flags |= O_TRUNC;
    if (H5F_ACC_CREAT & flags) o_flags |= O_CREAT;
    if (H5F_ACC_EXCL & flags) o_flags |= O_EXCL;

    /* Flag for Direct I/O */
    o_flags |= O_DIRECT;

    /* Open the file */
    if ((fd=HDopen(name, o_flags, 0666))<0)
        HSYS_GOTO_ERROR(H5E_FILE, H5E_CANTOPENFILE, NULL, "unable to open file")

    if (HDfstat(fd, &sb)<0)
        HSYS_GOTO_ERROR(H5E_FILE, H5E_BADFILE, NULL, "unable to fstat file")

    /* Create the new file struct */
    if (NULL==(file=H5FL_CALLOC(H5FD_direct_t)))
        HGOTO_ERROR(H5E_RESOURCE, H5E_NOSPACE, NULL, "unable to allocate file struct")

    /* Get the driver specific information */
    if(NULL == (plist = H5P_object_verify(fapl_id,H5P_FILE_ACCESS)))
        HGOTO_ERROR(H5E_ARGS, H5E_BADTYPE, NULL, "not a file access property list")
    fa = H5P_get_driver_info(plist);

    file->fd = fd;
    H5_ASSIGN_OVERFLOW(file->eof,sb.st_size,h5_stat_size_t,haddr_t);
    file->pos = HADDR_UNDEF;
    file->op = OP_UNKNOWN;
#ifdef _WIN32
    filehandle = _get_osfhandle(fd);
    (void)GetFileInformationByHandle((HANDLE)filehandle, &fileinfo);
    file->fileindexhi = fileinfo.nFileIndexHigh;
    file->fileindexlo = fileinfo.nFileIndexLow;
#else
    file->device = sb.st_dev;
#ifdef H5_VMS
    file->inode[0] = sb.st_ino[0];
    file->inode[1] = sb.st_ino[1];
    file->inode[2] = sb.st_ino[2];
#else
    file->inode = sb.st_ino;
#endif /*H5_VMS*/
#endif /*_WIN32*/
    file->fa.mboundary = fa->mboundary;
    file->fa.fbsize = fa->fbsize;
    file->fa.cbsize = fa->cbsize;

    /* Try to decide if data alignment is required.  The reason to check it here
     * is to handle correctly the case that the file is in a different file system
     * than the one where the program is running.
     */
    buf1 = (int*)HDmalloc(sizeof(int));
    if (HDposix_memalign(&buf2, file->fa.mboundary, file->fa.fbsize) != 0)
	HGOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "HDposix_memalign failed")

    if(o_flags &= O_CREAT) {
        if(write(file->fd, (void*)buf1, sizeof(int))<0) {
            if(write(file->fd, (void*)buf2, file->fa.fbsize)<0)
                HGOTO_ERROR(H5E_FILE, H5E_WRITEERROR, NULL, "file system may not support Direct I/O")
            else
                file->fa.must_align = TRUE;
        } else {
            file->fa.must_align = FALSE;
            file_truncate(file->fd, (file_offset_t)0);
        }
    } else {
        if(read(file->fd, (void*)buf1, sizeof(int))<0) {
            if(read(file->fd, (void*)buf2, file->fa.fbsize)<0)
                HGOTO_ERROR(H5E_FILE, H5E_READERROR, NULL, "file system may not support Direct I/O")
            else
                file->fa.must_align = TRUE;
        } else
            file->fa.must_align = FALSE;
    }

    if(buf1)
        HDfree(buf1);
    if(buf2)
        HDfree(buf2);

    /* Set return value */
    ret_value=(H5FD_t*)file;

done:
    if(ret_value==NULL) {
        if(fd>=0)
            HDclose(fd);
    } /* end if */

    FUNC_LEAVE_NOAPI(ret_value)
}


/*-------------------------------------------------------------------------
 * Function:	H5FD_direct_close
 *
 * Purpose:	Closes the file.
 *
 * Return:	Success:	0
 *
 *		Failure:	-1, file not closed.
 *
 * Programmer:	Raymond Lu
 *              Wednesday, 20 September 2006
 *
 * Modifications:
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5FD_direct_close(H5FD_t *_file)
{
    H5FD_direct_t	*file = (H5FD_direct_t*)_file;
    herr_t      	ret_value=SUCCEED;       /* Return value */

    FUNC_ENTER_NOAPI(H5FD_direct_close, FAIL)

    if (HDclose(file->fd)<0)
        HSYS_GOTO_ERROR(H5E_IO, H5E_CANTCLOSEFILE, FAIL, "unable to close file")

    H5FL_FREE(H5FD_direct_t,file);

done:
    FUNC_LEAVE_NOAPI(ret_value)
}


/*-------------------------------------------------------------------------
 * Function:	H5FD_direct_cmp
 *
 * Purpose:	Compares two files belonging to this driver using an
 *		arbitrary (but consistent) ordering.
 *
 * Return:	Success:	A value like strcmp()
 *
 *		Failure:	never fails (arguments were checked by the
 *				caller).
 *
 * Programmer:	Raymond Lu
 *              Thursday, 21 September 2006
 *
 * Modifications:
 *
 *-------------------------------------------------------------------------
 */
static int
H5FD_direct_cmp(const H5FD_t *_f1, const H5FD_t *_f2)
{
    const H5FD_direct_t	*f1 = (const H5FD_direct_t*)_f1;
    const H5FD_direct_t	*f2 = (const H5FD_direct_t*)_f2;
    int ret_value=0;

    FUNC_ENTER_NOAPI(H5FD_direct_cmp, H5FD_VFD_DEFAULT)

#ifdef _WIN32
    if (f1->fileindexhi < f2->fileindexhi) HGOTO_DONE(-1)
    if (f1->fileindexhi > f2->fileindexhi) HGOTO_DONE(1)

    if (f1->fileindexlo < f2->fileindexlo) HGOTO_DONE(-1)
    if (f1->fileindexlo > f2->fileindexlo) HGOTO_DONE(1)

#else
#ifdef H5_DEV_T_IS_SCALAR
    if (f1->device < f2->device) HGOTO_DONE(-1)
    if (f1->device > f2->device) HGOTO_DONE(1)
#else /* H5_DEV_T_IS_SCALAR */
    /* If dev_t isn't a scalar value on this system, just use memcmp to
     * determine if the values are the same or not.  The actual return value
     * shouldn't really matter...
     */
    if(HDmemcmp(&(f1->device),&(f2->device),sizeof(dev_t))<0) HGOTO_DONE(-1)
    if(HDmemcmp(&(f1->device),&(f2->device),sizeof(dev_t))>0) HGOTO_DONE(1)
#endif /* H5_DEV_T_IS_SCALAR */

#ifndef H5_VMS
    if (f1->inode < f2->inode) HGOTO_DONE(-1)
    if (f1->inode > f2->inode) HGOTO_DONE(1)
#else
    if(HDmemcmp(&(f1->inode),&(f2->inode),3*sizeof(ino_t))<0) HGOTO_DONE(-1)
    if(HDmemcmp(&(f1->inode),&(f2->inode),3*sizeof(ino_t))>0) HGOTO_DONE(1)
#endif /*H5_VMS*/

#endif

done:
    FUNC_LEAVE_NOAPI(ret_value)
}


/*-------------------------------------------------------------------------
 * Function:	H5FD_direct_query
 *
 * Purpose:	Set the flags that this VFL driver is capable of supporting.
 *              (listed in H5FDpublic.h)
 *
 * Return:	Success:	non-negative
 *
 *		Failure:	negative
 *
 * Programmer:	Raymond Lu
 *              Thursday, 21 September 2006
 *
 * Modifications:
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5FD_direct_query(const H5FD_t UNUSED * _f, unsigned long *flags /* out */)
{
    herr_t ret_value=SUCCEED;

    FUNC_ENTER_NOAPI(H5FD_direct_query, FAIL)

    /* Set the VFL feature flags that this driver supports */
    if(flags) {
        *flags = 0;
        *flags|=H5FD_FEAT_AGGREGATE_METADATA; /* OK to aggregate metadata allocations */
        *flags|=H5FD_FEAT_ACCUMULATE_METADATA; /* OK to accumulate metadata for faster writes */
        *flags|=H5FD_FEAT_DATA_SIEVE;       /* OK to perform data sieving for faster raw data reads & writes */
        *flags|=H5FD_FEAT_AGGREGATE_SMALLDATA; /* OK to aggregate "small" raw data allocations */
    }

done:
    FUNC_LEAVE_NOAPI(ret_value)
}


/*-------------------------------------------------------------------------
 * Function:	H5FD_direct_get_eoa
 *
 * Purpose:	Gets the end-of-address marker for the file. The EOA marker
 *		is the first address past the last byte allocated in the
 *		format address space.
 *
 * Return:	Success:	The end-of-address marker.
 *
 *		Failure:	HADDR_UNDEF
 *
 * Programmer:	Raymond Lu
 *              Wednesday, 20 September 2006
 *
 * Modifications:
 *              Raymond Lu
 *              21 Dec. 2006
 *              Added the parameter TYPE.  It's only used for MULTI driver.
 *
 *-------------------------------------------------------------------------
 */
static haddr_t
H5FD_direct_get_eoa(const H5FD_t *_file, H5FD_mem_t UNUSED type)
{
    const H5FD_direct_t	*file = (const H5FD_direct_t*)_file;
    haddr_t ret_value;  /* Return value */

    FUNC_ENTER_NOAPI(H5FD_direct_get_eoa, HADDR_UNDEF)

    /* Set return value */
    ret_value=file->eoa;

done:
    FUNC_LEAVE_NOAPI(ret_value)
}


/*-------------------------------------------------------------------------
 * Function:	H5FD_direct_set_eoa
 *
 * Purpose:	Set the end-of-address marker for the file. This function is
 *		called shortly after an existing HDF5 file is opened in order
 *		to tell the driver where the end of the HDF5 data is located.
 *
 * Return:	Success:	0
 *
 *		Failure:	-1
 *
 * Programmer:	Raymond Lu
 *              Wednesday, 20 September 2006
 *
 * Modifications:
 *              Raymond Lu
 *              21 Dec. 2006
 *              Added the parameter TYPE.  It's only used for MULTI driver.
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5FD_direct_set_eoa(H5FD_t *_file, H5FD_mem_t UNUSED type, haddr_t addr)
{
    H5FD_direct_t	*file = (H5FD_direct_t*)_file;
    herr_t ret_value=SUCCEED;   /* Return value */

    FUNC_ENTER_NOAPI(H5FD_direct_set_eoa, FAIL)

    file->eoa = addr;

done:
    FUNC_LEAVE_NOAPI(ret_value)
}


/*-------------------------------------------------------------------------
 * Function:	H5FD_direct_get_eof
 *
 * Purpose:	Returns the end-of-file marker, which is the greater of
 *		either the Unix end-of-file or the HDF5 end-of-address
 *		markers.
 *
 * Return:	Success:	End of file address, the first address past
 *				the end of the "file", either the Unix file
 *				or the HDF5 file.
 *
 *		Failure:	HADDR_UNDEF
 *
 * Programmer:	Raymond Lu
 *              Wednesday, 20 September 2006
 *
 * Modifications:
 *
 *-------------------------------------------------------------------------
 */
static haddr_t
H5FD_direct_get_eof(const H5FD_t *_file)
{
    const H5FD_direct_t	*file = (const H5FD_direct_t*)_file;
    haddr_t ret_value;  /* Return value */

    FUNC_ENTER_NOAPI(H5FD_direct_get_eof, HADDR_UNDEF)

    /* Set return value */
    ret_value=MAX(file->eof, file->eoa);

done:
    FUNC_LEAVE_NOAPI(ret_value)
}


/*-------------------------------------------------------------------------
 * Function:       H5FD_diect_get_handle
 *
 * Purpose:        Returns the file handle of direct file driver.
 *
 * Returns:        Non-negative if succeed or negative if fails.
 *
 * Programmer:     Raymond Lu
 *                 21 September 2006
 *
 * Modifications:
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5FD_direct_get_handle(H5FD_t *_file, hid_t UNUSED fapl, void** file_handle)
{
    H5FD_direct_t       *file = (H5FD_direct_t *)_file;
    herr_t              ret_value = SUCCEED;

    FUNC_ENTER_NOAPI(H5FD_direct_get_handle, FAIL)

    if(!file_handle)
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "file handle not valid")
    *file_handle = &(file->fd);

done:
    FUNC_LEAVE_NOAPI(ret_value)
}


/*-------------------------------------------------------------------------
 * Function:	H5FD_direct_read
 *
 * Purpose:	Reads SIZE bytes of data from FILE beginning at address ADDR
 *		into buffer BUF according to data transfer properties in
 *		DXPL_ID.
 *
 * Return:	Success:	Zero. Result is stored in caller-supplied
 *				buffer BUF.
 *
 *		Failure:	-1, Contents of buffer BUF are undefined.
 *
 * Programmer:	Raymond Lu
 *              Thursday, 21 September 2006
 *
 * Modifications:
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5FD_direct_read(H5FD_t *_file, H5FD_mem_t UNUSED type, hid_t UNUSED dxpl_id, haddr_t addr,
	       size_t size, void *buf/*out*/)
{
    H5FD_direct_t	*file = (H5FD_direct_t*)_file;
    ssize_t		nbytes;
    hbool_t		_must_align = TRUE;
    herr_t      	ret_value=SUCCEED;       /* Return value */
    size_t		alloc_size;
    void		*copy_buf, *p2, *p3;
    size_t		_boundary;
    size_t		_fbsize;
    size_t		_cbsize;
    haddr_t		copy_addr = addr;
    size_t		copy_size = size;

    FUNC_ENTER_NOAPI(H5FD_direct_read, FAIL)

    assert(file && file->pub.cls);
    assert(buf);

    /* Check for overflow conditions */
    if (HADDR_UNDEF==addr)
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "addr undefined")
    if (REGION_OVERFLOW(addr, size))
        HGOTO_ERROR(H5E_ARGS, H5E_OVERFLOW, FAIL, "addr overflow")
    if (addr+size>file->eoa)
        HGOTO_ERROR(H5E_ARGS, H5E_OVERFLOW, FAIL, "addr overflow")

    /* If the system doesn't require data to be aligned, read the data in
     * the same way as sec2 driver.
     */
    _must_align = file->fa.must_align;

    /* Get the memory boundary for alignment, file system block size, and maximal
     * copy buffer size.
     */
    _boundary = file->fa.mboundary;
    _fbsize = file->fa.fbsize;
    _cbsize = file->fa.cbsize;

    /* if the data is aligned or the system doesn't require data to be aligned,
     * read it directly from the file.  If not, read a bigger
     * and aligned data first, then copy the data into memory buffer.
     */
    if(!_must_align || ((addr%_fbsize==0) && (size%_fbsize==0) && ((size_t)buf%_boundary==0))) {
	    /* Seek to the correct location */
	    if ((addr!=file->pos || OP_READ!=file->op) &&
		    file_seek(file->fd, (file_offset_t)addr, SEEK_SET)<0)
		HSYS_GOTO_ERROR(H5E_IO, H5E_SEEKERROR, FAIL, "unable to seek to proper position")
 	    /* Read the aligned data in file first, being careful of interrupted
	     * system calls and partial results. */
	    while (size>0) {
		do {
		    nbytes = HDread(file->fd, buf, size);
		} while (-1==nbytes && EINTR==errno);
		if (-1==nbytes) /* error */
		    HSYS_GOTO_ERROR(H5E_IO, H5E_READERROR, FAIL, "file read failed")
		if (0==nbytes) {
		    /* end of file but not end of format address space */
		    HDmemset(buf, 0, size);
		    break;
		}
		assert(nbytes>=0);
		assert((size_t)nbytes<=size);
		H5_CHECK_OVERFLOW(nbytes,ssize_t,size_t);
		size -= (size_t)nbytes;
		H5_CHECK_OVERFLOW(nbytes,ssize_t,haddr_t);
		addr += (haddr_t)nbytes;
		buf = (char*)buf + nbytes;
	    }
    } else {
	    /* allocate memory needed for the Direct IO option up to the maximal
	     * copy buffer size. Make a bigger buffer for aligned I/O if size is
	     * smaller than maximal copy buffer. */
	    if(size < _cbsize)
	    	alloc_size = ((size / _fbsize) * _fbsize) + _fbsize;
	    else
	    	alloc_size = _cbsize;
	    if (HDposix_memalign(&copy_buf, _boundary, alloc_size) != 0)
		HGOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "HDposix_memalign failed")

	    /* look for the aligned position for reading the data */
	    if(file_seek(file->fd, (file_offset_t)(copy_addr - copy_addr % _fbsize), SEEK_SET) < 0)
		HSYS_GOTO_ERROR(H5E_IO, H5E_SEEKERROR, FAIL, "unable to seek to proper position")

	    /*
	     * Read the aligned data in file into aligned buffer first, then copy the data
	     * into the final buffer.  If the data size is bigger than maximal copy buffer
	     * size, do the reading by segment (the outer while loop).  If not, do one step
	     * reading.
	     */
	    p3 = buf;
	    do {
 	    	/* Read the aligned data in file first.  Not able to handle interrupted
		 * system calls and partial results like sec2 driver does because the
		 * data may no longer be aligned. It's expecially true when the data in
		 * file is smaller than ALLOC_SIZE. */
	    	HDmemset(copy_buf, 0, alloc_size);

	    	do {
		    nbytes = HDread(file->fd, copy_buf, alloc_size);
	    	} while(-1==nbytes && EINTR==errno);

	    	if (-1==nbytes) /* error */
		    HSYS_GOTO_ERROR(H5E_IO, H5E_READERROR, FAIL, "file read failed")

	    	/* look for the right position and copy the data to the original buffer.
		 * Consider all possible situations here: file address is not aligned on
		 * file block size; the end of data address is not aligned; the end of data
		 * address is aligned; data size is smaller or bigger than maximal copy size.*/
	    	p2 = (unsigned char*)copy_buf + (size_t)(copy_addr % _fbsize);
	    	if(size < _cbsize)
	    	    HDmemcpy(p3, p2, size);
	    	else if(size >= _cbsize && copy_size <= (alloc_size-(size_t)(copy_addr%_fbsize)))
	    	    HDmemcpy(p3, p2, copy_size);
	    	else if(size >= _cbsize && copy_size > (alloc_size-(size_t)(copy_addr%_fbsize))) {
	    	    HDmemcpy(p3, p2, (alloc_size - (size_t)(copy_addr % _fbsize)));
	    	    p3 = (unsigned char*)p3 + (alloc_size - (size_t)(copy_addr % _fbsize));
	    	}

		/* update the size and address of data being read. */
	    	if(copy_size > (alloc_size - (size_t)(copy_addr % _fbsize)))
	    	    copy_size -= (alloc_size - (size_t)(copy_addr % _fbsize));
	    	else
		    copy_size = 0;
	    	copy_addr += (alloc_size - (size_t)(copy_addr % _fbsize));
	    } while (copy_size > 0);

	    /*Final step: update address and buffer*/
	    addr += (haddr_t)size;
	    buf = (unsigned char*)buf + size;

	    if(copy_buf)
	    	HDfree(copy_buf);
    }

    /* Update current position */
    file->pos = addr;
    file->op = OP_READ;

done:
    if(ret_value<0) {
        /* Reset last file I/O information */
        file->pos = HADDR_UNDEF;
        file->op = OP_UNKNOWN;
    } /* end if */

    FUNC_LEAVE_NOAPI(ret_value)
}


/*-------------------------------------------------------------------------
 * Function:	H5FD_direct_write
 *
 * Purpose:	Writes SIZE bytes of data to FILE beginning at address ADDR
 *		from buffer BUF according to data transfer properties in
 *		DXPL_ID.
 *
 * Return:	Success:	Zero
 *
 *		Failure:	-1
 *
 * Programmer:	Raymond Lu
 *              Thursday, 21 September 2006
 *
 * Modifications:
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5FD_direct_write(H5FD_t *_file, H5FD_mem_t UNUSED type, hid_t UNUSED dxpl_id, haddr_t addr,
		size_t size, const void *buf)
{
    H5FD_direct_t	*file = (H5FD_direct_t*)_file;
    ssize_t		nbytes;
    hbool_t		_must_align = TRUE;
    herr_t      	ret_value=SUCCEED;       /* Return value */
    size_t		alloc_size;
    void		*copy_buf, *p1;
    const void		*p3;
    size_t		_boundary;
    size_t		_fbsize;
    size_t		_cbsize;
    haddr_t             copy_addr = addr;
    size_t             	copy_size = size;

    FUNC_ENTER_NOAPI(H5FD_direct_write, FAIL)

    assert(file && file->pub.cls);
    assert(buf);

    /* Check for overflow conditions */
    if (HADDR_UNDEF==addr)
        HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "addr undefined")
    if (REGION_OVERFLOW(addr, size))
        HGOTO_ERROR(H5E_ARGS, H5E_OVERFLOW, FAIL, "addr overflow")
    if (addr+size>file->eoa)
        HGOTO_ERROR(H5E_ARGS, H5E_OVERFLOW, FAIL, "addr overflow")

    /* If the system doesn't require data to be aligned, read the data in
     * the same way as sec2 driver.
     */
    _must_align = file->fa.must_align;

    /* Get the memory boundary for alignment, file system block size, and maximal
     * copy buffer size.
     */
    _boundary = file->fa.mboundary;
    _fbsize = file->fa.fbsize;
    _cbsize = file->fa.cbsize;

    /* if the data is aligned or the system doesn't require data to be aligned,
     * write it directly to the file.  If not, read a bigger and aligned data
     * first, update buffer with user data, then write the data out.
     */
    if(!_must_align || ((addr%_fbsize==0) && (size%_fbsize==0) && ((size_t)buf%_boundary==0))) {
	    /* Seek to the correct location */
	    if ((addr!=file->pos || OP_WRITE!=file->op) &&
		    file_seek(file->fd, (file_offset_t)addr, SEEK_SET)<0)
		HSYS_GOTO_ERROR(H5E_IO, H5E_SEEKERROR, FAIL, "unable to seek to proper position")

	    while (size>0) {
		do {
		    nbytes = HDwrite(file->fd, buf, size);
		} while (-1==nbytes && EINTR==errno);
		if (-1==nbytes) /* error */
		    HSYS_GOTO_ERROR(H5E_IO, H5E_WRITEERROR, FAIL, "file write failed")
		assert(nbytes>0);
		assert((size_t)nbytes<=size);
		H5_CHECK_OVERFLOW(nbytes,ssize_t,size_t);
		size -= (size_t)nbytes;
		H5_CHECK_OVERFLOW(nbytes,ssize_t,haddr_t);
		addr += (haddr_t)nbytes;
		buf = (const char*)buf + nbytes;
	    }
    } else {
	    /* allocate memory needed for the Direct IO option up to the maximal
	     * copy buffer size. Make a bigger buffer for aligned I/O if size is
	     * smaller than maximal copy buffer.
	     */
	    if(size < _cbsize)
	    	alloc_size = ((size / _fbsize) * _fbsize) + _fbsize;
	    else
		alloc_size = _cbsize;

	    if (HDposix_memalign(&copy_buf, _boundary, alloc_size) != 0)
		HGOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, FAIL, "HDposix_memalign failed")

	    /* look for the right position for reading the data */
	    if(file_seek(file->fd, (file_offset_t)(copy_addr - copy_addr % _fbsize), SEEK_SET) < 0)
		HSYS_GOTO_ERROR(H5E_IO, H5E_SEEKERROR, FAIL, "unable to seek to proper position")

	    p3 = buf;
	    do {
	    	/*
	     	 * Read the aligned data first if the aligned region doesn't fall
	    	 * entirely in the range to be writen.  Not able to handle interrupted
		 * system calls and partial results like sec2 driver does because the
		 * data may no longer be aligned. It's expecially true when the data in
		 * file is smaller than ALLOC_SIZE.
	    	 */
	    	HDmemset(copy_buf, 0, alloc_size);

		if(copy_addr <= addr || (copy_addr + alloc_size) >= (addr + size)) {
		    do {
		    	nbytes = read(file->fd, copy_buf, alloc_size);
		    } while (-1==nbytes && EINTR==errno);

		    if (-1==nbytes) /* error */
		    	HSYS_GOTO_ERROR(H5E_IO, H5E_READERROR, FAIL, "file read failed")
		}

	    	/* look for the right position and append or copy the data to be written to
	     	 * the aligned buffer.
            	 * Consider all possible situations here: file address is not aligned on
            	 * file block size; the end of data address is not aligned; the end of data
            	 * address is aligned; data size is smaller or bigger than maximal copy size.
	    	 */
	    	p1 = (unsigned char*)copy_buf + (size_t)(copy_addr % _fbsize);
	    	if(size < _cbsize)
	    	    HDmemcpy(p1, p3, size);
	    	else if(size >= _cbsize && copy_size <= (alloc_size-(size_t)(copy_addr%_fbsize))) {
		    HDmemcpy(p1, p3, copy_size);
	    	}else if(size >= _cbsize && copy_size > (alloc_size-(size_t)(copy_addr%_fbsize))) {
		    HDmemcpy(p1, p3, (alloc_size - (size_t)(copy_addr % _fbsize)));
		    p3 = (const unsigned char *)p3 + (alloc_size - (size_t)(copy_addr % _fbsize));
	    	}

	    	/*look for the aligned position for writing the data*/
	    	if(file_seek(file->fd, (file_offset_t)(copy_addr - copy_addr % _fbsize), SEEK_SET) < 0)
		    HSYS_GOTO_ERROR(H5E_IO, H5E_SEEKERROR, FAIL, "unable to seek to proper position")

	    	/*
	     	 * Write the data. It doesn't truncate the extra data introduced by
		 * alignment because that step is done in H5FD_direct_flush.
	    	 */
		do {
		    nbytes = HDwrite(file->fd, copy_buf, alloc_size);
		} while (-1==nbytes && EINTR==errno);

		if (-1==nbytes) /* error */
		    HSYS_GOTO_ERROR(H5E_IO, H5E_WRITEERROR, FAIL, "file write failed")

            	/* update the size and address of data being read. */
            	if(copy_size > (alloc_size - (size_t)(copy_addr % _fbsize)))
                    copy_size -= (alloc_size - (size_t)(copy_addr % _fbsize));
            	else
                    copy_size = 0;

            	copy_addr += (alloc_size - (size_t)(copy_addr % _fbsize));
	} while (copy_size > 0);

	/*Update the address and size*/
	addr += (haddr_t)size;
	buf = (const char*)buf + size;

	if(copy_buf)
	    HDfree(copy_buf);
    }

    /* Update current position and eof */
    file->pos = addr;
    file->op = OP_WRITE;
    if (file->pos>file->eof)
        file->eof = file->pos;

done:
    if(ret_value<0) {
        /* Reset last file I/O information */
        file->pos = HADDR_UNDEF;
        file->op = OP_UNKNOWN;
    } /* end if */

    FUNC_LEAVE_NOAPI(ret_value)
}


/*-------------------------------------------------------------------------
 * Function:	H5FD_direct_truncate
 *
 * Purpose:	Makes sure that the true file size is the same (or larger)
 *		than the end-of-address.
 *
 * Return:	Success:	Non-negative
 *
 *		Failure:	Negative
 *
 * Programmer:	Raymond Lu
 *              Thursday, 21 September 2006
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5FD_direct_truncate(H5FD_t *_file, hid_t UNUSED dxpl_id, hbool_t UNUSED closing)
{
    H5FD_direct_t	*file = (H5FD_direct_t*)_file;
    herr_t      	ret_value = SUCCEED;       /* Return value */

    FUNC_ENTER_NOAPI(H5FD_direct_truncate, FAIL)

    assert(file);

    /* Extend the file to make sure it's large enough */
    if (file->eoa!=file->eof) {
#ifdef _WIN32
        HFILE filehandle;   /* Windows file handle */
        LARGE_INTEGER li;   /* 64-bit integer for SetFilePointer() call */

        /* Map the posix file handle to a Windows file handle */
        filehandle = _get_osfhandle(file->fd);

        /* Translate 64-bit integers into form Windows wants */
        /* [This algorithm is from the Windows documentation for SetFilePointer()] */
        li.QuadPart = (LONGLONG)file->eoa;
        (void)SetFilePointer((HANDLE)filehandle,li.LowPart,&li.HighPart,FILE_BEGIN);
        if(SetEndOfFile((HANDLE)filehandle)==0)
            HGOTO_ERROR(H5E_IO, H5E_SEEKERROR, FAIL, "unable to extend file properly")
#else /* _WIN32 */
        if (-1==file_truncate(file->fd, (file_offset_t)file->eoa))
            HSYS_GOTO_ERROR(H5E_IO, H5E_SEEKERROR, FAIL, "unable to extend file properly")
#endif /* _WIN32 */

        /* Update the eof value */
        file->eof = file->eoa;

        /* Reset last file I/O information */
        file->pos = HADDR_UNDEF;
        file->op = OP_UNKNOWN;
    }
    else if (file->fa.must_align){
	/*Even though eof is equal to eoa, file is still truncated because Direct I/O
	 *write introduces some extra data for alignment.
	 */
        if (-1==file_truncate(file->fd, (file_offset_t)file->eof))
            HSYS_GOTO_ERROR(H5E_IO, H5E_SEEKERROR, FAIL, "unable to extend file properly")
    }

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5FD_direct_truncate() */
#endif /* H5_HAVE_DIRECT */

