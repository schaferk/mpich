/*
 * Copyright (C) by Argonne National Laboratory
 *     See COPYRIGHT in top-level directory
 */

#include "mpi.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* tests MPI_MODE_EXCL */

static void handle_error(int errcode, const char *str)
{
    char msg[MPI_MAX_ERROR_STRING];
    int resultlen;
    MPI_Error_string(errcode, msg, &resultlen);
    fprintf(stderr, "%s: %s\n", str, msg);
}

int main(int argc, char **argv)
{
    MPI_File fh;
    int rank, len, err, i;
    int errs = 0, toterrs;
    char *filename;

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

/* process 0 takes the file name as a command-line argument and
   broadcasts it to other processes */
    if (!rank) {
        i = 1;
        while ((i < argc) && strcmp("-fname", *argv)) {
            i++;
            argv++;
        }
        if (i >= argc) {
            fprintf(stderr, "\n*#  Usage: excl -fname filename\n\n");
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        argv++;
        len = strlen(*argv);
        filename = (char *) malloc(len + 10);
        strcpy(filename, *argv);
        MPI_Bcast(&len, 1, MPI_INT, 0, MPI_COMM_WORLD);
        MPI_Bcast(filename, len + 10, MPI_CHAR, 0, MPI_COMM_WORLD);
    } else {
        MPI_Bcast(&len, 1, MPI_INT, 0, MPI_COMM_WORLD);
        filename = (char *) malloc(len + 10);
        MPI_Bcast(filename, len + 10, MPI_CHAR, 0, MPI_COMM_WORLD);
    }


    if (!rank) {
        err = MPI_File_delete(filename, MPI_INFO_NULL);
        if (err != MPI_SUCCESS) {
            int errorclass;
            MPI_Error_class(err, &errorclass);
            if (errorclass != MPI_ERR_NO_SUCH_FILE) {   /* ignore this error class */
                fprintf(stderr, "Process %d: fails to delete file %s\n", rank, filename);
                errs++;
            }
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);

    /* this open should succeed */
    err = MPI_File_open(MPI_COMM_WORLD, filename,
                        MPI_MODE_CREATE | MPI_MODE_EXCL | MPI_MODE_RDWR, MPI_INFO_NULL, &fh);
    if (err != MPI_SUCCESS) {
        handle_error(err, "MPI_File_open");
        errs++;
        fprintf(stderr, "Process %d: open failed when it should have succeeded\n", rank);
    } else
        MPI_File_close(&fh);

    MPI_Barrier(MPI_COMM_WORLD);

    /* this open should fail */
    err = MPI_File_open(MPI_COMM_WORLD, filename,
                        MPI_MODE_CREATE | MPI_MODE_EXCL | MPI_MODE_RDWR, MPI_INFO_NULL, &fh);
    if (err == MPI_SUCCESS) {
        errs++;
        fprintf(stderr, "Process %d: open succeeded when it should have failed\n", rank);
        MPI_File_close(&fh);
    }

    MPI_Allreduce(&errs, &toterrs, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    if (rank == 0) {
        if (toterrs > 0) {
            fprintf(stderr, "Found %d errors\n", toterrs);
        } else {
            fprintf(stdout, " No Errors\n");
        }
    }

    free(filename);
    MPI_Finalize();
    return (toterrs > 0);
}
