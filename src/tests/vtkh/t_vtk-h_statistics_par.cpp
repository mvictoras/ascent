//-----------------------------------------------------------------------------
///
/// file: t_vtk-h_dataset.cpp
///
//-----------------------------------------------------------------------------

#include "gtest/gtest.h"

#include <vtkh/vtkh.hpp>
#include <vtkh/DataSet.hpp>
#include <vtkh/filters/Statistics.hpp>
#include "t_vtkm_test_utils.hpp"

#include <iostream>
#include <mpi.h>

//----------------------------------------------------------------------------
TEST(vtkh_statistics_par, vtkh_stats)
{
#ifdef VTKM_ENABLE_KOKKOS
  vtkh::InitializeKokkos();
#endif

  MPI_Init(NULL, NULL);
  int comm_size, rank;
  MPI_Comm_size(MPI_COMM_WORLD, &comm_size);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  vtkh::SetMPICommHandle(MPI_Comm_c2f(MPI_COMM_WORLD));
  vtkh::DataSet data_set;

  const int base_size = 32;
  const int blocks_per_rank = 2;
  const int num_blocks = comm_size * blocks_per_rank;

  for(int i = 0; i < blocks_per_rank; ++i)
  {
    int domain_id = rank * blocks_per_rank + i;
    data_set.AddDomain(CreateTestData(domain_id, num_blocks, base_size), domain_id);
  }

  vtkh::DataSet* res;
  vtkh::Statistics stats;

  stats.SetField("point_data_Float64");
  stats.SetInput(&data_set);
  stats.Update();
  res = stats.GetOutput();


  if(rank == 0) res->PrintSummary(std::cout);

  MPI_Finalize();
}
