// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <mpi.h>
#include "Transceiver.hpp"

class MPITransceiver : public Transceiver
{
public:
    MPITransceiver();
    ~MPITransceiver();

    rank_type nranks() const
    {
        return _nranks;
    }

    rank_type rank() const
    {
        return _rank;
    }

    MPI_Comm comm() const
    {
        return _comm;
    }
    

    virtual void barrier();
    virtual void bcast(void * ptr, size_t N, rank_type root);
    virtual void reduce_all(void * inout, DTypeId T, size_t N, RedOpType op);
    virtual void alltoall(const void* buffer_send,
                          const int* counts_send,
                          const int* displacements_send,
                          DTypeId datatype_send,
                          void* buffer_recv,
                          const int* counts_recv,
                          const int* displacements_recv,
                          DTypeId datatype_recv);
    virtual void gather(void* buffer,
                        const int* counts,
                        const int* displacements,
                        DTypeId datatype,
                        rank_type root);
    virtual void send_recv(void* buffer_send,
                           int count_send,
                           DTypeId datatype_send,
                           int dest,
                           int source);

private:
    rank_type _nranks, _rank;
    MPI_Comm _comm;
};
