/*
 * Copyright (C) by Argonne National Laboratory
 *     See COPYRIGHT in top-level directory
 */

#include <stdlib.h>
#include <stdio.h>
#include <mpi.h>
#include "mpitest.h"

#define BUFSIZE (128*1024)

int main(int argc, char *argv[])
{
    int i, rank, size;
    int *buf;
    MPI_Win win;

    MTest_Init(&argc, &argv);

    buf = malloc(BUFSIZE);
    MTEST_VG_MEM_INIT(buf, BUFSIZE);

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (size != 2) {
        printf("test must be run with 2 processes!\n");
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    if (rank == 0)
        MPI_Win_create(buf, BUFSIZE, sizeof(int), MPI_INFO_NULL, MPI_COMM_WORLD, &win);
    else
        MPI_Win_create(MPI_BOTTOM, 0, 1, MPI_INFO_NULL, MPI_COMM_WORLD, &win);

    MPI_Win_fence(0, win);

    int num_iter = 1000;
    if (MTestGetStressLevel()) {
        num_iter = 100000;
    }
    if (rank == 1) {
        for (i = 0; i < num_iter; i++)
            MPI_Get(buf, BUFSIZE / sizeof(int), MPI_INT, 0, 0, BUFSIZE / sizeof(int), MPI_INT, win);
    }

    MPI_Win_fence(0, win);
    MPI_Win_free(&win);

    free(buf);
    MTest_Finalize(0);

    return 0;
}
