/*
 * Copyright (C) by Argonne National Laboratory
 *     See COPYRIGHT in top-level directory
 */

#include "adio.h"
#include "adio_extern.h"

#ifdef AGGREGATION_PROFILE
#include "mpe.h"
#endif

/* prototypes of functions used for collective writes only. */
static void ADIOI_Exch_and_write(ADIO_File fd, void *buf, MPI_Datatype
                                 datatype, int nprocs, int myrank,
                                 ADIOI_Access
                                 * others_req, ADIO_Offset * offset_list,
                                 ADIO_Offset * len_list, MPI_Count contig_access_count, ADIO_Offset
                                 min_st_offset, ADIO_Offset fd_size,
                                 ADIO_Offset * fd_start, ADIO_Offset * fd_end,
                                 MPI_Aint * buf_idx, int *error_code);
static void ADIOI_W_Exchange_data(ADIO_File fd, void *buf, char *write_buf, ADIOI_Flatlist_node * flat_buf, ADIO_Offset * offset_list, ADIO_Offset * len_list, MPI_Count * send_size, MPI_Count * recv_size, ADIO_Offset off, MPI_Count size,   /* 10 */
                                  MPI_Count * count, MPI_Count * start_pos,
                                  MPI_Count * partial_recv, MPI_Count * sent_to_proc, int nprocs,
                                  int myrank, int buftype_is_contig, MPI_Count contig_access_count,
                                  ADIO_Offset min_st_offset, ADIO_Offset fd_size,
                                  ADIO_Offset * fd_start, ADIO_Offset * fd_end,
                                  ADIOI_Access * others_req, MPI_Count * send_buf_idx,
                                  MPI_Count * curr_to_proc, MPI_Count * done_to_proc, int *hole,
                                  int iter, MPI_Aint buftype_extent, MPI_Aint * buf_idx,
                                  int *error_code);
void ADIOI_Fill_send_buffer(ADIO_File fd, void *buf, ADIOI_Flatlist_node * flat_buf,
                            char **send_buf, ADIO_Offset * offset_list, ADIO_Offset * len_list,
                            MPI_Count * send_size, MPI_Request * requests, MPI_Count * sent_to_proc,
                            int nprocs, int myrank, MPI_Count contig_access_count,
                            ADIO_Offset min_st_offset, ADIO_Offset fd_size, ADIO_Offset * fd_start,
                            ADIO_Offset * fd_end, MPI_Count * send_buf_idx,
                            MPI_Count * curr_to_proc, MPI_Count * done_to_proc, int iter,
                            MPI_Aint buftype_extent);

void ADIOI_GEN_WriteStridedColl(ADIO_File fd, const void *buf, MPI_Aint count,
                                MPI_Datatype datatype, int file_ptr_type,
                                ADIO_Offset offset, ADIO_Status * status, int
                                *error_code)
{
/* Uses a generalized version of the extended two-phase method described
   in "An Extended Two-Phase Method for Accessing Sections of
   Out-of-Core Arrays", Rajeev Thakur and Alok Choudhary,
   Scientific Programming, (5)4:301--317, Winter 1996.
   http://www.mcs.anl.gov/home/thakur/ext2ph.ps */

    ADIOI_Access *my_req;
    /* array of nprocs access structures, one for each other process in
     * whose file domain this process's request lies */

    ADIOI_Access *others_req;
    /* array of nprocs access structures, one for each other process
     * whose request lies in this process's file domain. */

    int i, filetype_is_contig, nprocs, nprocs_for_coll, myrank;
    MPI_Count contig_access_count = 0;
    int interleave_count = 0, buftype_is_contig;
    MPI_Count *count_my_req_per_proc, count_my_req_procs;
    MPI_Count *count_others_req_per_proc, count_others_req_procs;
    ADIO_Offset orig_fp, start_offset, end_offset, fd_size, min_st_offset, off;
    ADIO_Offset *offset_list = NULL, *st_offsets = NULL, *fd_start = NULL,
        *fd_end = NULL, *end_offsets = NULL;
    MPI_Aint *buf_idx = NULL;
    ADIO_Offset *len_list = NULL;
    int old_error, tmp_error;

    if (fd->hints->cb_pfr != ADIOI_HINT_DISABLE) {
        /* Cast away const'ness as the below function is used for read
         * and write */
        ADIOI_IOStridedColl(fd, (char *) buf, count, ADIOI_WRITE, datatype,
                            file_ptr_type, offset, status, error_code);
        return;
    }

    MPI_Comm_size(fd->comm, &nprocs);
    MPI_Comm_rank(fd->comm, &myrank);

/* the number of processes that actually perform I/O, nprocs_for_coll,
 * is stored in the hints off the ADIO_File structure
 */
    nprocs_for_coll = fd->hints->cb_nodes;
    orig_fp = fd->fp_ind;

    /* only check for interleaving if cb_write isn't disabled */
    if (fd->hints->cb_write != ADIOI_HINT_DISABLE) {
        /* For this process's request, calculate the list of offsets and
         * lengths in the file and determine the start and end offsets. */

        /* Note: end_offset points to the last byte-offset that will be accessed.
         * e.g., if start_offset=0 and 100 bytes to be read, end_offset=99 */

        ADIOI_Calc_my_off_len(fd, count, datatype, file_ptr_type, offset,
                              &offset_list, &len_list, &start_offset,
                              &end_offset, &contig_access_count);

        /* each process communicates its start and end offsets to other
         * processes. The result is an array each of start and end offsets stored
         * in order of process rank. */

        st_offsets = (ADIO_Offset *) ADIOI_Malloc(nprocs * 2 * sizeof(ADIO_Offset));
        end_offsets = st_offsets + nprocs;

        MPI_Allgather(&start_offset, 1, ADIO_OFFSET, st_offsets, 1, ADIO_OFFSET, fd->comm);
        MPI_Allgather(&end_offset, 1, ADIO_OFFSET, end_offsets, 1, ADIO_OFFSET, fd->comm);

        /* are the accesses of different processes interleaved? */
        for (i = 1; i < nprocs; i++)
            if ((st_offsets[i] < end_offsets[i - 1]) && (st_offsets[i] <= end_offsets[i]))
                interleave_count++;
        /* This is a rudimentary check for interleaving, but should suffice
         * for the moment. */
    }

    ADIOI_Datatype_iscontig(datatype, &buftype_is_contig);

    if (fd->hints->cb_write == ADIOI_HINT_DISABLE ||
        (!interleave_count && (fd->hints->cb_write == ADIOI_HINT_AUTO))) {
        /* use independent accesses */
        if (fd->hints->cb_write != ADIOI_HINT_DISABLE) {
            ADIOI_Free(offset_list);
            ADIOI_Free(st_offsets);
        }

        fd->fp_ind = orig_fp;
        ADIOI_Datatype_iscontig(fd->filetype, &filetype_is_contig);

        if (buftype_is_contig && filetype_is_contig) {
            if (file_ptr_type == ADIO_EXPLICIT_OFFSET) {
                off = fd->disp + (ADIO_Offset) (fd->etype_size) * offset;
                ADIO_WriteContig(fd, buf, count, datatype,
                                 ADIO_EXPLICIT_OFFSET, off, status, error_code);
            } else
                ADIO_WriteContig(fd, buf, count, datatype, ADIO_INDIVIDUAL, 0, status, error_code);
        } else
            ADIO_WriteStrided(fd, buf, count, datatype, file_ptr_type, offset, status, error_code);

        return;
    }

/* Divide the I/O workload among "nprocs_for_coll" processes. This is
   done by (logically) dividing the file into file domains (FDs); each
   process may directly access only its own file domain. */

    ADIOI_Calc_file_domains(st_offsets, end_offsets, nprocs,
                            nprocs_for_coll, &min_st_offset,
                            &fd_start, &fd_end,
                            fd->hints->min_fdomain_size, &fd_size, fd->hints->striping_unit);


/* calculate what portions of the access requests of this process are
   located in what file domains */

    ADIOI_Calc_my_req(fd, offset_list, len_list, contig_access_count,
                      min_st_offset, fd_start, fd_end, fd_size,
                      nprocs, &count_my_req_procs, &count_my_req_per_proc, &my_req, &buf_idx);

/* based on everyone's my_req, calculate what requests of other
   processes lie in this process's file domain.
   count_others_req_procs = number of processes whose requests lie in
   this process's file domain (including this process itself)
   count_others_req_per_proc[i] indicates how many separate contiguous
   requests of proc. i lie in this process's file domain. */

    ADIOI_Calc_others_req(fd, count_my_req_procs,
                          count_my_req_per_proc, my_req,
                          nprocs, myrank, &count_others_req_procs, &count_others_req_per_proc,
                          &others_req);

/* exchange data and write in sizes of no more than coll_bufsize. */
    /* Cast away const'ness for the below function */
    ADIOI_Exch_and_write(fd, (char *) buf, datatype, nprocs, myrank,
                         others_req, offset_list,
                         len_list, contig_access_count, min_st_offset,
                         fd_size, fd_start, fd_end, buf_idx, error_code);

    /* If this collective write is followed by an independent write,
     * it's possible to have those subsequent writes on other processes
     * race ahead and sneak in before the read-modify-write completes.
     * We carry out a collective communication at the end here so no one
     * can start independent i/o before collective I/O completes.
     *
     * need to do some gymnastics with the error codes so that if something
     * went wrong, all processes report error, but if a process has a more
     * specific error code, we can still have that process report the
     * additional information */

    old_error = *error_code;
    if (*error_code != MPI_SUCCESS)
        *error_code = MPI_ERR_IO;

    /* optimization: if only one process performing i/o, we can perform
     * a less-expensive Bcast  */
#ifdef ADIOI_MPE_LOGGING
    MPE_Log_event(ADIOI_MPE_postwrite_a, 0, NULL);
#endif
    if (fd->hints->cb_nodes == 1)
        MPI_Bcast(error_code, 1, MPI_INT, fd->hints->ranklist[0], fd->comm);
    else {
        tmp_error = *error_code;
        MPI_Allreduce(&tmp_error, error_code, 1, MPI_INT, MPI_MAX, fd->comm);
    }
#ifdef ADIOI_MPE_LOGGING
    MPE_Log_event(ADIOI_MPE_postwrite_b, 0, NULL);
#endif
#ifdef AGGREGATION_PROFILE
    MPE_Log_event(5012, 0, NULL);
#endif

    if ((old_error != MPI_SUCCESS) && (old_error != MPI_ERR_IO))
        *error_code = old_error;

    /* free all memory allocated for collective I/O */
    ADIOI_Free_my_req(nprocs, count_my_req_per_proc, my_req, buf_idx);
    ADIOI_Free_others_req(nprocs, count_others_req_per_proc, others_req);

    ADIOI_Free(offset_list);
    ADIOI_Free(st_offsets);
    ADIOI_Free(fd_start);

#ifdef HAVE_STATUS_SET_BYTES
    if (status) {
        MPI_Count bufsize, size;
        /* Don't set status if it isn't needed */
        MPI_Type_size_x(datatype, &size);
        bufsize = size * count;
        MPIR_Status_set_bytes(status, datatype, bufsize);
    }
/* This is a temporary way of filling in status. The right way is to
   keep track of how much data was actually written during collective I/O. */
#endif

    fd->fp_sys_posn = -1;       /* set it to null. */
#ifdef AGGREGATION_PROFILE
    MPE_Log_event(5013, 0, NULL);
#endif
}



/* If successful, error_code is set to MPI_SUCCESS.  Otherwise an error
 * code is created and returned in error_code.
 */
static void ADIOI_Exch_and_write(ADIO_File fd, void *buf, MPI_Datatype
                                 datatype, int nprocs,
                                 int myrank,
                                 ADIOI_Access
                                 * others_req, ADIO_Offset * offset_list,
                                 ADIO_Offset * len_list, MPI_Count contig_access_count,
                                 ADIO_Offset min_st_offset, ADIO_Offset fd_size,
                                 ADIO_Offset * fd_start, ADIO_Offset * fd_end,
                                 MPI_Aint * buf_idx, int *error_code)
{
/* Send data to appropriate processes and write in sizes of no more
   than coll_bufsize.
   The idea is to reduce the amount of extra memory required for
   collective I/O. If all data were written all at once, which is much
   easier, it would require temp space more than the size of user_buf,
   which is often unacceptable. For example, to write a distributed
   array to a file, where each local array is 8Mbytes, requiring
   at least another 8Mbytes of temp space is unacceptable. */

    /* Not convinced end_loc-st_loc couldn't be > int, so make these offsets */
    ADIO_Offset size = 0;
    int hole, i, m, ntimes, max_ntimes, buftype_is_contig;
    ADIO_Offset st_loc = -1, end_loc = -1, off, done, req_off;
    char *write_buf = NULL;
    MPI_Count *curr_offlen_ptr, *send_size, *count, req_len, *recv_size;
    MPI_Count *partial_recv, *sent_to_proc, *start_pos;
    int flag;
    MPI_Count *send_buf_idx, *curr_to_proc, *done_to_proc;
    MPI_Status status;
    ADIOI_Flatlist_node *flat_buf = NULL;
    MPI_Aint lb, buftype_extent;
    int info_flag;
    MPI_Aint coll_bufsize;
    char *value;
    static char myname[] = "ADIOI_EXCH_AND_WRITE";

    *error_code = MPI_SUCCESS;  /* changed below if error */
    /* only I/O errors are currently reported */

/* calculate the number of writes of size coll_bufsize
   to be done by each process and the max among all processes.
   That gives the no. of communication phases as well. */

    value = (char *) ADIOI_Malloc((MPI_MAX_INFO_VAL + 1) * sizeof(char));
    ADIOI_Info_get(fd->info, "cb_buffer_size", MPI_MAX_INFO_VAL, value, &info_flag);
    coll_bufsize = atoi(value);
    ADIOI_Free(value);


    for (i = 0; i < nprocs; i++) {
        if (others_req[i].count) {
            st_loc = others_req[i].offsets[0];
            end_loc = others_req[i].offsets[0];
            break;
        }
    }

    for (i = 0; i < nprocs; i++)
        for (MPI_Count j = 0; j < others_req[i].count; j++) {
            st_loc = MPL_MIN(st_loc, others_req[i].offsets[j]);
            end_loc = MPL_MAX(end_loc, (others_req[i].offsets[j]
                                        + others_req[i].lens[j] - 1));
        }

/* ntimes=ceiling_div(end_loc - st_loc + 1, coll_bufsize)*/

    ntimes = (int) ((end_loc - st_loc + coll_bufsize) / coll_bufsize);

    if ((st_loc == -1) && (end_loc == -1)) {
        ntimes = 0;     /* this process does no writing. */
    }

    MPI_Allreduce(&ntimes, &max_ntimes, 1, MPI_INT, MPI_MAX, fd->comm);

    write_buf = fd->io_buf;

    curr_offlen_ptr = ADIOI_Calloc(nprocs * 10, sizeof(*curr_offlen_ptr));
    /* its use is explained below. calloc initializes to 0. */

    count = curr_offlen_ptr + nprocs;
    /* to store count of how many off-len pairs per proc are satisfied
     * in an iteration. */

    partial_recv = count + nprocs;
    /* if only a portion of the last off-len pair is recd. from a process
     * in a particular iteration, the length recd. is stored here.
     * calloc initializes to 0. */

    send_size = partial_recv + nprocs;
    /* total size of data to be sent to each proc. in an iteration.
     * Of size nprocs so that I can use MPI_Alltoall later. */

    recv_size = send_size + nprocs;
    /* total size of data to be recd. from each proc. in an iteration. */

    sent_to_proc = recv_size + nprocs;
    /* amount of data sent to each proc so far. Used in
     * ADIOI_Fill_send_buffer. initialized to 0 here. */

    send_buf_idx = sent_to_proc + nprocs;
    curr_to_proc = send_buf_idx + nprocs;
    done_to_proc = curr_to_proc + nprocs;
    /* Above three are used in ADIOI_Fill_send_buffer */

    start_pos = done_to_proc + nprocs;
    /* used to store the starting value of curr_offlen_ptr[i] in
     * this iteration */

    ADIOI_Datatype_iscontig(datatype, &buftype_is_contig);
    if (!buftype_is_contig) {
        flat_buf = ADIOI_Flatten_and_find(datatype);
    }
    MPI_Type_get_extent(datatype, &lb, &buftype_extent);


/* I need to check if there are any outstanding nonblocking writes to
   the file, which could potentially interfere with the writes taking
   place in this collective write call. Since this is not likely to be
   common, let me do the simplest thing possible here: Each process
   completes all pending nonblocking operations before completing. */

    /*ADIOI_Complete_async(error_code);
     * if (*error_code != MPI_SUCCESS) return;
     * MPI_Barrier(fd->comm);
     */

    done = 0;
    off = st_loc;

    for (m = 0; m < ntimes; m++) {
        /* go through all others_req and check which will be satisfied
         * by the current write */

        /* Note that MPI guarantees that displacements in filetypes are in
         * monotonically nondecreasing order and that, for writes, the
         * filetypes cannot specify overlapping regions in the file. This
         * simplifies implementation a bit compared to reads. */

        /* off = start offset in the file for the data to be written in
         * this iteration
         * size = size of data written (bytes) corresponding to off
         * req_off = off in file for a particular contiguous request
         * minus what was satisfied in previous iteration
         * req_size = size corresponding to req_off */

        /* first calculate what should be communicated */

        for (i = 0; i < nprocs; i++)
            count[i] = recv_size[i] = 0;

        size = MPL_MIN(coll_bufsize, end_loc - st_loc + 1 - done);

        for (i = 0; i < nprocs; i++) {
            if (others_req[i].count) {
                start_pos[i] = curr_offlen_ptr[i];
                MPI_Count j;
                for (j = curr_offlen_ptr[i]; j < others_req[i].count; j++) {
                    if (partial_recv[i]) {
                        /* this request may have been partially
                         * satisfied in the previous iteration. */
                        req_off = others_req[i].offsets[j] + partial_recv[i];
                        req_len = others_req[i].lens[j] - partial_recv[i];
                        partial_recv[i] = 0;
                        /* modify the off-len pair to reflect this change */
                        others_req[i].offsets[j] = req_off;
                        others_req[i].lens[j] = req_len;
                    } else {
                        req_off = others_req[i].offsets[j];
                        req_len = others_req[i].lens[j];
                    }
                    if (req_off < off + size) {
                        count[i]++;
                        ADIOI_Assert((((ADIO_Offset) (uintptr_t) write_buf) + req_off - off) ==
                                     (ADIO_Offset) (uintptr_t) (write_buf + req_off - off));
                        MPI_Aint addr;
                        MPI_Get_address(write_buf + req_off - off, &addr);
                        others_req[i].mem_ptrs[j] = addr;
                        recv_size[i] += (MPL_MIN(off + size - req_off, req_len));

                        if (off + size - req_off < req_len) {
                            partial_recv[i] = (off + size - req_off);

                            /* --BEGIN ERROR HANDLING-- */
                            if ((j + 1 < others_req[i].count) &&
                                (others_req[i].offsets[j + 1] < off + size)) {
                                *error_code = MPIO_Err_create_code(MPI_SUCCESS,
                                                                   MPIR_ERR_RECOVERABLE,
                                                                   myname,
                                                                   __LINE__,
                                                                   MPI_ERR_ARG,
                                                                   "Filetype specifies overlapping write regions (which is illegal according to the MPI-2 specification)",
                                                                   0);
                                /* allow to continue since additional
                                 * communication might have to occur
                                 */
                            }
                            /* --END ERROR HANDLING-- */
                            break;
                        }
                    } else
                        break;
                }
                curr_offlen_ptr[i] = j;
            }
        }

        ADIOI_W_Exchange_data(fd, buf, write_buf, flat_buf, offset_list,
                              len_list, send_size, recv_size, off, size, count,
                              start_pos, partial_recv,
                              sent_to_proc, nprocs, myrank,
                              buftype_is_contig, contig_access_count,
                              min_st_offset, fd_size, fd_start, fd_end,
                              others_req, send_buf_idx, curr_to_proc,
                              done_to_proc, &hole, m, buftype_extent, buf_idx, error_code);
        if (*error_code != MPI_SUCCESS)
            return;

        flag = 0;
        for (i = 0; i < nprocs; i++)
            if (count[i])
                flag = 1;

        if (flag) {
            ADIO_WriteContig(fd, write_buf, size, MPI_BYTE, ADIO_EXPLICIT_OFFSET,
                             off, &status, error_code);
            if (*error_code != MPI_SUCCESS)
                return;
        }

        off += size;
        done += size;
    }

    for (i = 0; i < nprocs; i++)
        count[i] = recv_size[i] = 0;
    for (m = ntimes; m < max_ntimes; m++) {
        /* nothing to recv, but check for send. */
        ADIOI_W_Exchange_data(fd, buf, write_buf, flat_buf, offset_list,
                              len_list, send_size, recv_size, off, size, count,
                              start_pos, partial_recv,
                              sent_to_proc, nprocs, myrank,
                              buftype_is_contig, contig_access_count,
                              min_st_offset, fd_size, fd_start, fd_end,
                              others_req, send_buf_idx,
                              curr_to_proc, done_to_proc, &hole, m,
                              buftype_extent, buf_idx, error_code);
        if (*error_code != MPI_SUCCESS)
            return;
    }

    ADIOI_Free(curr_offlen_ptr);
}


/* Sets error_code to MPI_SUCCESS if successful, or creates an error code
 * in the case of error.
 */
static void ADIOI_W_Exchange_data(ADIO_File fd, void *buf, char *write_buf,
                                  ADIOI_Flatlist_node * flat_buf, ADIO_Offset
                                  * offset_list, ADIO_Offset * len_list, MPI_Count * send_size,
                                  MPI_Count * recv_size, ADIO_Offset off, MPI_Count size,
                                  MPI_Count * count, MPI_Count * start_pos,
                                  MPI_Count * partial_recv,
                                  MPI_Count * sent_to_proc, int nprocs,
                                  int myrank, int
                                  buftype_is_contig, MPI_Count contig_access_count,
                                  ADIO_Offset min_st_offset,
                                  ADIO_Offset fd_size,
                                  ADIO_Offset * fd_start, ADIO_Offset * fd_end,
                                  ADIOI_Access * others_req,
                                  MPI_Count * send_buf_idx, MPI_Count * curr_to_proc,
                                  MPI_Count * done_to_proc, int *hole, int iter,
                                  MPI_Aint buftype_extent, MPI_Aint * buf_idx, int *error_code)
{
    int i, j, nprocs_recv, nprocs_send, err;
    MPI_Count *tmp_len;
    char **send_buf = NULL;
    MPI_Request *requests, *send_req;
    MPI_Datatype *recv_types;
    MPI_Status *statuses, status;
    MPI_Count *srt_len = NULL;
    MPI_Count sum;
    ADIO_Offset *srt_off = NULL;
    static char myname[] = "ADIOI_W_EXCHANGE_DATA";

/* exchange recv_size info so that each process knows how much to
   send to whom. */

    MPI_Alltoall(recv_size, 1, MPI_COUNT, send_size, 1, MPI_COUNT, fd->comm);

    /* create derived datatypes for recv */

    nprocs_send = 0;
    nprocs_recv = 0;
    sum = 0;
    for (i = 0; i < nprocs; i++) {
        sum += count[i];
        if (recv_size[i])
            nprocs_recv++;
        if (send_size[i])
            nprocs_send++;
    }

    recv_types = (MPI_Datatype *)
        ADIOI_Malloc((nprocs_recv + 1) * sizeof(MPI_Datatype));
/* +1 to avoid a 0-size malloc */

    tmp_len = ADIOI_Malloc(nprocs * sizeof(*tmp_len));
    j = 0;
    for (i = 0; i < nprocs; i++) {
        if (recv_size[i]) {
/* take care if the last off-len pair is a partial recv */
            if (partial_recv[i]) {
                MPI_Count k = start_pos[i] + count[i] - 1;
                tmp_len[i] = others_req[i].lens[k];
                others_req[i].lens[k] = partial_recv[i];
            }
            ADIOI_Type_create_hindexed_x(count[i],
                                         &(others_req[i].lens[start_pos[i]]),
                                         &(others_req[i].mem_ptrs[start_pos[i]]),
                                         MPI_BYTE, recv_types + j);
            /* absolute displacements; use MPI_BOTTOM in recv */
            MPI_Type_commit(recv_types + j);
            j++;
        }
    }

    /* To avoid a read-modify-write, check if there are holes in the
     * data to be written. For this, merge the (sorted) offset lists
     * others_req using a heap-merge. */

    /* valgrind-detcted optimization: if there is no work on this process we do
     * not need to search for holes */
    if (sum) {
        srt_off = (ADIO_Offset *) ADIOI_Malloc(sum * sizeof(ADIO_Offset));
        srt_len = ADIOI_Malloc(sum * sizeof(*srt_len));

        ADIOI_Heap_merge(others_req, count, srt_off, srt_len, start_pos, nprocs, nprocs_recv, sum);
    }

    /* for partial recvs, restore original lengths */
    for (i = 0; i < nprocs; i++)
        if (partial_recv[i]) {
            MPI_Count k = start_pos[i] + count[i] - 1;
            others_req[i].lens[k] = tmp_len[i];
        }
    ADIOI_Free(tmp_len);

    /* check if there are any holes. If yes, must do read-modify-write.
     * holes can be in three places.  'middle' is what you'd expect: the
     * processes are operating on noncontigous data.  But holes can also show
     * up at the beginning or end of the file domain (see John Bent ROMIO REQ
     * #835). Missing these holes would result in us writing more data than
     * received by everyone else. */

    *hole = 0;
    if (sum) {
        if (off != srt_off[0])  /* hole at the front */
            *hole = 1;
        else {  /* coalesce the sorted offset-length pairs */
            for (i = 1; i < sum; i++) {
                if (srt_off[i] <= srt_off[0] + srt_len[0]) {
                    MPI_Count new_len = srt_off[i] + srt_len[i] - srt_off[0];
                    if (new_len > srt_len[0])
                        srt_len[0] = new_len;
                } else
                    break;
            }
            if (i < sum || size != srt_len[0])  /* hole in middle or end */
                *hole = 1;
        }

        ADIOI_Free(srt_off);
        ADIOI_Free(srt_len);
    }

    if (nprocs_recv) {
        if (*hole) {
            ADIO_ReadContig(fd, write_buf, size, MPI_BYTE,
                            ADIO_EXPLICIT_OFFSET, off, &status, &err);
            /* --BEGIN ERROR HANDLING-- */
            if (err != MPI_SUCCESS) {
                *error_code = MPIO_Err_create_code(err,
                                                   MPIR_ERR_RECOVERABLE, myname,
                                                   __LINE__, MPI_ERR_IO, "**ioRMWrdwr", 0);
                return;
            }
            /* --END ERROR HANDLING-- */
        }
    }

    if (fd->atomicity) {
        /* bug fix from Wei-keng Liao and Kenin Coloma */
        requests = (MPI_Request *)
            ADIOI_Malloc((nprocs_send + 1) * sizeof(MPI_Request));
        send_req = requests;
    } else {
        requests = (MPI_Request *)
            ADIOI_Malloc((nprocs_send + nprocs_recv + 1) * sizeof(MPI_Request));
        /* +1 to avoid a 0-size malloc */

        /* post receives */
        j = 0;
        for (i = 0; i < nprocs; i++) {
            if (recv_size[i]) {
                MPI_Irecv(MPI_BOTTOM, 1, recv_types[j], i, ADIOI_COLL_TAG(i, iter),
                          fd->comm, requests + j);
                j++;
            }
        }
        send_req = requests + nprocs_recv;
    }

/* post sends. if buftype_is_contig, data can be directly sent from
   user buf at location given by buf_idx. else use send_buf. */

#ifdef AGGREGATION_PROFILE
    MPE_Log_event(5032, 0, NULL);
#endif
    if (buftype_is_contig) {
        j = 0;
        for (i = 0; i < nprocs; i++)
            if (send_size[i]) {
                MPI_Isend_c(((char *) buf) + buf_idx[i], send_size[i],
                            MPI_BYTE, i, ADIOI_COLL_TAG(i, iter), fd->comm, send_req + j);
                j++;
                buf_idx[i] += send_size[i];
            }
    } else if (nprocs_send) {
        /* buftype is not contig */
        size_t msgLen = 0;
        for (i = 0; i < nprocs; i++)
            msgLen += send_size[i];
        send_buf = (char **) ADIOI_Malloc(nprocs * sizeof(char *));
        send_buf[0] = (char *) ADIOI_Malloc(msgLen * sizeof(char));
        for (i = 1; i < nprocs; i++)
            send_buf[i] = send_buf[i - 1] + send_size[i - 1];

        ADIOI_Fill_send_buffer(fd, buf, flat_buf, send_buf,
                               offset_list, len_list, send_size,
                               send_req,
                               sent_to_proc, nprocs, myrank,
                               contig_access_count,
                               min_st_offset, fd_size, fd_start, fd_end,
                               send_buf_idx, curr_to_proc, done_to_proc, iter, buftype_extent);
        /* the send is done in ADIOI_Fill_send_buffer */
    }

    if (fd->atomicity) {
        /* bug fix from Wei-keng Liao and Kenin Coloma */
        j = 0;
        for (i = 0; i < nprocs; i++) {
            MPI_Status wkl_status;
            if (recv_size[i]) {
                MPI_Recv(MPI_BOTTOM, 1, recv_types[j], i, ADIOI_COLL_TAG(i, iter), fd->comm,
                         &wkl_status);
                j++;
            }
        }
    }

    for (i = 0; i < nprocs_recv; i++)
        MPI_Type_free(recv_types + i);
    ADIOI_Free(recv_types);

#ifdef MPI_STATUSES_IGNORE
    statuses = MPI_STATUSES_IGNORE;
#else
    if (fd->atomicity) {
        /* bug fix from Wei-keng Liao and Kenin Coloma */
        statuses = (MPI_Status *) ADIOI_Malloc((nprocs_send + 1) * sizeof(MPI_Status));
        /* +1 to avoid a 0-size malloc */
    } else {
        statuses = (MPI_Status *) ADIOI_Malloc((nprocs_send + nprocs_recv + 1) *
                                               sizeof(MPI_Status));
        /* +1 to avoid a 0-size malloc */
    }
#endif

#ifdef NEEDS_MPI_TEST
    i = 0;
    if (fd->atomicity) {
        /* bug fix from Wei-keng Liao and Kenin Coloma */
        while (!i)
            MPI_Testall(nprocs_send, send_req, &i, statuses);
    } else {
        while (!i)
            MPI_Testall(nprocs_send + nprocs_recv, requests, &i, statuses);
    }
#else
    if (fd->atomicity)
        /* bug fix from Wei-keng Liao and Kenin Coloma */
        MPI_Waitall(nprocs_send, send_req, statuses);
    else
        MPI_Waitall(nprocs_send + nprocs_recv, requests, statuses);
#endif

#ifdef AGGREGATION_PROFILE
    MPE_Log_event(5033, 0, NULL);
#endif
#ifndef MPI_STATUSES_IGNORE
    ADIOI_Free(statuses);
#endif
    ADIOI_Free(requests);
    if (!buftype_is_contig && nprocs_send) {
        ADIOI_Free(send_buf[0]);
        ADIOI_Free(send_buf);
    }
}

#define ADIOI_BUF_INCR \
{ \
    while (buf_incr) { \
        size_in_buf = MPL_MIN(buf_incr, flat_buf_sz); \
        user_buf_idx += size_in_buf; \
        flat_buf_sz -= size_in_buf; \
        if (!flat_buf_sz) { \
            if (flat_buf_idx < (flat_buf->count - 1)) flat_buf_idx++; \
            else { \
                flat_buf_idx = 0; \
                n_buftypes++; \
            } \
            user_buf_idx = flat_buf->indices[flat_buf_idx] + \
                              (ADIO_Offset)n_buftypes*(ADIO_Offset)buftype_extent; \
            flat_buf_sz = flat_buf->blocklens[flat_buf_idx]; \
        } \
        buf_incr -= size_in_buf; \
    } \
}


#define ADIOI_BUF_COPY \
{ \
    while (size) { \
        size_in_buf = MPL_MIN(size, flat_buf_sz); \
  ADIOI_Assert((((ADIO_Offset)(uintptr_t)buf) + user_buf_idx) == (ADIO_Offset)(uintptr_t)((uintptr_t)buf + user_buf_idx)); \
  ADIOI_Assert(size_in_buf == (size_t)size_in_buf); \
        memcpy(&(send_buf[p][send_buf_idx[p]]), \
               ((char *) buf) + user_buf_idx, size_in_buf); \
        send_buf_idx[p] += size_in_buf; \
        user_buf_idx += size_in_buf; \
        flat_buf_sz -= size_in_buf; \
        if (!flat_buf_sz) { \
            if (flat_buf_idx < (flat_buf->count - 1)) flat_buf_idx++; \
            else { \
                flat_buf_idx = 0; \
                n_buftypes++; \
            } \
            user_buf_idx = flat_buf->indices[flat_buf_idx] + \
                              (ADIO_Offset)n_buftypes*(ADIO_Offset)buftype_extent; \
            flat_buf_sz = flat_buf->blocklens[flat_buf_idx]; \
        } \
        size -= size_in_buf; \
        buf_incr -= size_in_buf; \
    } \
    ADIOI_BUF_INCR \
}





void ADIOI_Fill_send_buffer(ADIO_File fd, void *buf, ADIOI_Flatlist_node
                            * flat_buf, char **send_buf, ADIO_Offset
                            * offset_list, ADIO_Offset * len_list, MPI_Count * send_size,
                            MPI_Request * requests, MPI_Count * sent_to_proc,
                            int nprocs, int myrank,
                            MPI_Count contig_access_count,
                            ADIO_Offset min_st_offset, ADIO_Offset fd_size,
                            ADIO_Offset * fd_start, ADIO_Offset * fd_end,
                            MPI_Count * send_buf_idx, MPI_Count * curr_to_proc,
                            MPI_Count * done_to_proc, int iter, MPI_Aint buftype_extent)
{
/* this function is only called if buftype is not contig */

    int p, jj;
    ADIO_Offset flat_buf_idx, flat_buf_sz, size_in_buf, buf_incr, size;
    ADIO_Offset n_buftypes, off, len, rem_len, user_buf_idx;

/*  curr_to_proc[p] = amount of data sent to proc. p that has already
    been accounted for so far
    done_to_proc[p] = amount of data already sent to proc. p in
    previous iterations
    user_buf_idx = current location in user buffer
    send_buf_idx[p] = current location in send_buf of proc. p  */

    for (MPI_Count i = 0; i < nprocs; i++) {
        send_buf_idx[i] = curr_to_proc[i] = 0;
        done_to_proc[i] = sent_to_proc[i];
    }
    jj = 0;

    user_buf_idx = flat_buf->indices[0];
    flat_buf_idx = 0;
    n_buftypes = 0;
    flat_buf_sz = flat_buf->blocklens[0];

    /* flat_buf_idx = current index into flattened buftype
     * flat_buf_sz = size of current contiguous component in
     * flattened buf */

    for (MPI_Count i = 0; i < contig_access_count; i++) {
        off = offset_list[i];
        rem_len = len_list[i];

        /*this request may span the file domains of more than one process */
        while (rem_len != 0) {
            len = rem_len;
            /* NOTE: len value is modified by ADIOI_Calc_aggregator() to be no
             * longer than the single region that processor "p" is responsible
             * for.
             */
            p = ADIOI_Calc_aggregator(fd, off, min_st_offset, &len, fd_size, fd_start, fd_end);

            if (send_buf_idx[p] < send_size[p]) {
                if (curr_to_proc[p] + len > done_to_proc[p]) {
                    if (done_to_proc[p] > curr_to_proc[p]) {
                        size = MPL_MIN(curr_to_proc[p] + len -
                                       done_to_proc[p], send_size[p] - send_buf_idx[p]);
                        buf_incr = done_to_proc[p] - curr_to_proc[p];
                        ADIOI_BUF_INCR buf_incr = curr_to_proc[p] + len - done_to_proc[p];
                        /* ok to cast: bounded by cb buffer size */
                        curr_to_proc[p] = done_to_proc[p] + (int) size;
                    ADIOI_BUF_COPY} else {
                        size = MPL_MIN(len, send_size[p] - send_buf_idx[p]);
                        buf_incr = len;
                        curr_to_proc[p] += size;
                    ADIOI_BUF_COPY}
                    if (send_buf_idx[p] == send_size[p]) {
                        MPI_Isend_c(send_buf[p], send_size[p], MPI_BYTE, p,
                                    ADIOI_COLL_TAG(p, iter), fd->comm, requests + jj);
                        jj++;
                    }
                } else {
                    curr_to_proc[p] += len;
                    buf_incr = len;
                ADIOI_BUF_INCR}
            } else {
                buf_incr = len;
            ADIOI_BUF_INCR}
            off += len;
            rem_len -= len;
        }
    }
    for (int i = 0; i < nprocs; i++) {
        if (send_size[i]) {
            sent_to_proc[i] = curr_to_proc[i];
        }
    }
}
