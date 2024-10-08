/*
 * Copyright (C) by Argonne National Laboratory
 *     See COPYRIGHT in top-level directory
 */

#include "mpioimpl.h"
#include <limits.h>
#include <assert.h>

#ifdef HAVE_WEAK_SYMBOLS

#if defined(HAVE_PRAGMA_WEAK)
#pragma weak MPI_File_iread_shared = PMPI_File_iread_shared
#elif defined(HAVE_PRAGMA_HP_SEC_DEF)
#pragma _HP_SECONDARY_DEF PMPI_File_iread_shared MPI_File_iread_shared
#elif defined(HAVE_PRAGMA_CRI_DUP)
#pragma _CRI duplicate MPI_File_iread_shared as PMPI_File_iread_shared
/* end of weak pragmas */
#elif defined(HAVE_WEAK_ATTRIBUTE)
int MPI_File_iread_shared(MPI_File fh, void *buf, int count, MPI_Datatype datatype,
                          MPIO_Request * request)
    __attribute__ ((weak, alias("PMPI_File_iread_shared")));
#endif

#if defined(HAVE_PRAGMA_WEAK)
#pragma weak MPI_File_iread_shared_c = PMPI_File_iread_shared_c
#elif defined(HAVE_PRAGMA_HP_SEC_DEF)
#pragma _HP_SECONDARY_DEF PMPI_File_iread_shared_c MPI_File_iread_shared_c
#elif defined(HAVE_PRAGMA_CRI_DUP)
#pragma _CRI duplicate MPI_File_iread_shared_c as PMPI_File_iread_shared_c
/* end of weak pragmas */
#elif defined(HAVE_WEAK_ATTRIBUTE)
int MPI_File_iread_shared_c(MPI_File fh, void *buf, MPI_Count count, MPI_Datatype datatype,
                            MPIO_Request * request)
    __attribute__ ((weak, alias("PMPI_File_iread_shared_c")));
#endif

/* Include mapping from MPI->PMPI */
#define MPIO_BUILD_PROFILING
#include "mpioprof.h"
#endif

#ifdef HAVE_MPI_GREQUEST
#include "mpiu_greq.h"
#endif

/*@
    MPI_File_iread_shared - Nonblocking read using shared file pointer

Input Parameters:
. fh - file handle (handle)
. count - number of elements in buffer (nonnegative integer)
. datatype - datatype of each buffer element (handle)

Output Parameters:
. buf - initial address of buffer (choice)
. request - request object (handle)

.N fortran
@*/
int MPI_File_iread_shared(MPI_File fh, void *buf, int count,
                          MPI_Datatype datatype, MPI_Request * request)
{
    return MPIOI_File_iread_shared(fh, buf, count, datatype, request);
}

/* large count function */


/*@
    MPI_File_iread_shared_c - Nonblocking read using shared file pointer

Input Parameters:
. fh - file handle (handle)
. count - number of elements in buffer (nonnegative integer)
. datatype - datatype of each buffer element (handle)

Output Parameters:
. buf - initial address of buffer (choice)
. request - request object (handle)

.N fortran
@*/
int MPI_File_iread_shared_c(MPI_File fh, void *buf, MPI_Count count,
                            MPI_Datatype datatype, MPI_Request * request)
{
    return MPIOI_File_iread_shared(fh, buf, count, datatype, request);
}

#ifdef MPIO_BUILD_PROFILING
int MPIOI_File_iread_shared(MPI_File fh, void *buf, MPI_Aint count,
                            MPI_Datatype datatype, MPI_Request * request)
{
    int error_code, buftype_is_contig, filetype_is_contig;
    ADIO_Offset bufsize;
    ADIO_File adio_fh;
    static char myname[] = "MPI_FILE_IREAD_SHARED";
    MPI_Count datatype_size, incr;
    MPI_Status status;
    ADIO_Offset off, shared_fp;
    MPI_Offset nbytes = 0;
    void *xbuf = NULL, *e32_buf = NULL, *host_buf = NULL;

    ROMIO_THREAD_CS_ENTER();

    adio_fh = MPIO_File_resolve(fh);

    /* --BEGIN ERROR HANDLING-- */
    MPIO_CHECK_FILE_HANDLE(adio_fh, myname, error_code);
    MPIO_CHECK_COUNT(adio_fh, count, myname, error_code);
    MPIO_CHECK_DATATYPE(adio_fh, datatype, myname, error_code);
    /* --END ERROR HANDLING-- */

    MPI_Type_size_x(datatype, &datatype_size);

    /* --BEGIN ERROR HANDLING-- */
    MPIO_CHECK_INTEGRAL_ETYPE(adio_fh, count, datatype_size, myname, error_code);
    MPIO_CHECK_FS_SUPPORTS_SHARED(adio_fh, myname, error_code);
    MPIO_CHECK_COUNT_SIZE(adio_fh, count, datatype_size, myname, error_code);
    /* --END ERROR HANDLING-- */

    ADIOI_Datatype_iscontig(datatype, &buftype_is_contig);
    ADIOI_Datatype_iscontig(adio_fh->filetype, &filetype_is_contig);

    ADIOI_TEST_DEFERRED(adio_fh, myname, &error_code);

    incr = (count * datatype_size) / adio_fh->etype_size;
    ADIO_Get_shared_fp(adio_fh, incr, &shared_fp, &error_code);

    /* --BEGIN ERROR HANDLING-- */
    if (error_code != MPI_SUCCESS) {
        /* note: ADIO_Get_shared_fp should have set up error code already? */
        MPIO_Err_return_file(adio_fh, error_code);
    }
    /* --END ERROR HANDLING-- */

    xbuf = buf;
    if (adio_fh->is_external32) {
        MPI_Aint e32_size = 0;
        error_code = MPIU_datatype_full_size(datatype, &e32_size);
        if (error_code != MPI_SUCCESS)
            goto fn_exit;

        e32_buf = ADIOI_Malloc(e32_size * count);
        xbuf = e32_buf;
    } else {
        MPIO_GPU_HOST_ALLOC(host_buf, buf, count, datatype);
        if (host_buf != NULL) {
            xbuf = host_buf;
        }
    }

    if (buftype_is_contig && filetype_is_contig) {
        /* convert count and shared_fp to bytes */
        bufsize = datatype_size * count;
        off = adio_fh->disp + adio_fh->etype_size * shared_fp;
        if (!(adio_fh->atomicity)) {
            ADIO_IreadContig(adio_fh, xbuf, count, datatype, ADIO_EXPLICIT_OFFSET,
                             off, request, &error_code);
        } else {
            /* to maintain strict atomicity semantics with other concurrent
             * operations, lock (exclusive) and call blocking routine */

            if (adio_fh->file_system != ADIO_NFS) {
                ADIOI_WRITE_LOCK(adio_fh, off, SEEK_SET, bufsize);
            }

            ADIO_ReadContig(adio_fh, xbuf, count, datatype, ADIO_EXPLICIT_OFFSET,
                            off, &status, &error_code);

            if (adio_fh->file_system != ADIO_NFS) {
                ADIOI_UNLOCK(adio_fh, off, SEEK_SET, bufsize);
            }
            if (error_code == MPI_SUCCESS) {
                nbytes = count * datatype_size;
            }
            MPIO_Completed_request_create(&adio_fh, nbytes, &error_code, request);
        }
    } else {
        ADIO_IreadStrided(adio_fh, xbuf, count, datatype, ADIO_EXPLICIT_OFFSET,
                          shared_fp, request, &error_code);
    }

    /* --BEGIN ERROR HANDLING-- */
    if (error_code != MPI_SUCCESS)
        error_code = MPIO_Err_return_file(adio_fh, error_code);
    /* --END ERROR HANDLING-- */

    if (e32_buf != NULL) {
        error_code = MPIU_read_external32_conversion_fn(buf, datatype, count, e32_buf);
        ADIOI_Free(e32_buf);
    }

    MPIO_GPU_SWAP_BACK(host_buf, buf, count, datatype);

  fn_exit:
    ROMIO_THREAD_CS_EXIT();
    return error_code;
}
#endif
