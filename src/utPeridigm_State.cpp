/*! \file utPeridigm_State.cpp */

//@HEADER
// ************************************************************************
//
//                             Peridigm
//                 Copyright (2011) Sandia Corporation
//
// Under the terms of Contract DE-AC04-94AL85000 with Sandia Corporation,
// the U.S. Government retains certain rights in this software.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the Corporation nor the names of the
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY SANDIA CORPORATION "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SANDIA CORPORATION OR THE
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Questions?
// David J. Littlewood   djlittl@sandia.gov
// John A. Mitchell      jamitch@sandia.gov
// Michael L. Parks      mlparks@sandia.gov
// Stewart A. Silling    sasilli@sandia.gov
//
// ************************************************************************
//@HEADER

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_ALTERNATIVE_INIT_API
#include <boost/test/unit_test.hpp>
#include <Epetra_ConfigDefs.h> // used to define HAVE_MPI
#ifdef HAVE_MPI
  #include <Epetra_MpiComm.h>
#else
  #include <Epetra_SerialComm.h>
#endif
#include "Peridigm_State.hpp"
#include <vector>

using namespace boost::unit_test;
using namespace Teuchos;
using namespace PeridigmNS;

//! Create a State object for a two-point problem, check data storage and basic functionality.
void twoPointProblem()
{
  Teuchos::RCP<Epetra_Comm> comm;
  #ifdef HAVE_MPI
    comm = rcp(new Epetra_MpiComm(MPI_COMM_WORLD));
  #else
    comm = rcp(new Epetra_SerialComm);
  #endif
  int numProcs = comm->NumProc();

  // set up a hard-coded layout for two points
  int numCells = 2;

  // set up overlap maps, which include ghosted nodes
  int numGlobalElements(numCells), numMyElements, elementSize, indexBase(0);
  std::vector<int> myGlobalElements;
  if(numProcs == 1)
    numMyElements = 2;
  else if(numProcs == 2)
    numMyElements = 1;
  myGlobalElements.resize(numMyElements);
  for(int i=0; i<numMyElements ; ++i)
    myGlobalElements[i] = i;
  elementSize = 1;

  // oneDimensionalOverlapMap
  // used for cell volumes and scalar constitutive data
  Teuchos::RCP<Epetra_BlockMap> oneDimensionalOverlapMap =
    Teuchos::rcp(new Epetra_BlockMap(numGlobalElements, numMyElements, &myGlobalElements[0], elementSize, indexBase, *comm));
  // threeDimensionalOverlapMap
  // used for positions, displacements, velocities and vector constitutive data
  elementSize = 3;
  Teuchos::RCP<Epetra_BlockMap> threeDimensionalOverlapMap = 
    Teuchos::rcp(new Epetra_BlockMap(numGlobalElements, numMyElements, &myGlobalElements[0], elementSize, indexBase, *comm)); 
  // bondMap
  // used for bond damage and bond constitutive data
  std::vector<int> bondElementSize(numMyElements, 1);
  Teuchos::RCP<Epetra_BlockMap> bondMap = 
    Teuchos::rcp(new Epetra_BlockMap(numGlobalElements, numMyElements, &myGlobalElements[0], &bondElementSize[0], indexBase, *comm));

  // create a state object
  State state;

  // create a list of scalar field specs and allocate the data
  Teuchos::RCP< std::vector<Field_NS::FieldSpec> > scalarFieldSpecs = Teuchos::rcp(new std::vector<Field_NS::FieldSpec>);
  scalarFieldSpecs->push_back(Field_NS::DEFAULT_FIELDTYPE);
  scalarFieldSpecs->push_back(Field_NS::VOLUME);
  scalarFieldSpecs->push_back(Field_NS::ID);
  scalarFieldSpecs->push_back(Field_NS::PROC_NUM);
  scalarFieldSpecs->push_back(Field_NS::DAMAGE);
  scalarFieldSpecs->push_back(Field_NS::WEIGHTED_VOLUME);
  scalarFieldSpecs->push_back(Field_NS::DILATATION);
  scalarFieldSpecs->push_back(Field_NS::NUM_NEIGHBORS);
  scalarFieldSpecs->push_back(Field_NS::LAMBDA);
  scalarFieldSpecs->push_back(Field_NS::SHEAR_CORRECTION_FACTOR);
  state.allocateScalarData(scalarFieldSpecs, oneDimensionalOverlapMap);
  BOOST_CHECK_EQUAL( state.getScalarMultiVector()->NumVectors(), (int)scalarFieldSpecs->size() );
  BOOST_CHECK_EQUAL( state.getScalarMultiVector()->MyLength(), oneDimensionalOverlapMap->NumMyPoints() );
  BOOST_CHECK( state.getScalarMultiVector()->Map().SameAs( *oneDimensionalOverlapMap ) );

  // create a list of vector field specs and allocate the data
  Teuchos::RCP< std::vector<Field_NS::FieldSpec> > vectorFieldSpecs = Teuchos::rcp(new std::vector<Field_NS::FieldSpec>);
  vectorFieldSpecs->push_back(Field_NS::COORD3D);
  vectorFieldSpecs->push_back(Field_NS::DISPL3D);
  vectorFieldSpecs->push_back(Field_NS::CURCOORD3D);
  vectorFieldSpecs->push_back(Field_NS::VELOC3D);
  vectorFieldSpecs->push_back(Field_NS::ACCEL3D);
  vectorFieldSpecs->push_back(Field_NS::FORCE3D);
  vectorFieldSpecs->push_back(Field_NS::FORCE_DENSITY3D);
  vectorFieldSpecs->push_back(Field_NS::CONTACT_FORCE3D);
  vectorFieldSpecs->push_back(Field_NS::CONTACT_FORCE_DENSITY3D);
  state.allocateVectorData(vectorFieldSpecs, threeDimensionalOverlapMap);
  BOOST_CHECK_EQUAL( state.getVectorMultiVector()->NumVectors(), (int)vectorFieldSpecs->size() );
  BOOST_CHECK_EQUAL( state.getVectorMultiVector()->MyLength(), threeDimensionalOverlapMap->NumMyPoints() );
  BOOST_CHECK( state.getVectorMultiVector()->Map().SameAs( *threeDimensionalOverlapMap ) );

  // create a list of bond field specs and allocate the data
  Teuchos::RCP< std::vector<Field_NS::FieldSpec> > bondFieldSpecs = Teuchos::rcp(new std::vector<Field_NS::FieldSpec>);
  bondFieldSpecs->push_back(Field_NS::BOND_DAMAGE);
  bondFieldSpecs->push_back(Field_NS::DEVIATORIC_PLASTIC_EXTENSION);
  bondFieldSpecs->push_back(Field_NS::DEVIATORIC_BACK_EXTENSION);
  state.allocateBondData(bondFieldSpecs, bondMap);
  BOOST_CHECK_EQUAL( state.getBondMultiVector()->NumVectors(), (int)bondFieldSpecs->size() );
  BOOST_CHECK_EQUAL( state.getBondMultiVector()->MyLength(), bondMap->NumMyPoints() );
  BOOST_CHECK( state.getBondMultiVector()->Map().SameAs( *bondMap ) );

  // check initialization of data to zero
  Teuchos::RCP<Epetra_Vector> coordinates = state.getData(Field_NS::COORD3D);
  for(int i=0 ; i<coordinates->MyLength() ; ++i)
    BOOST_CHECK_EQUAL( (*coordinates)[i], 0.0 );

  // set some data
  {
    // scalar data
    Epetra_Vector& ids = *(state.getData(Field_NS::ID));
    for(int i=0 ; i<ids.MyLength() ; ++i)
      ids[i] = i;
    // vector data
    Epetra_Vector& force = *(state.getData(Field_NS::FORCE3D));
    for(int i=0 ; i<force.Map().NumMyElements() ; ++i){
      force[i*3] = i*3;
      force[i*3+1] = i*3+1;
      force[i*3+2] = i*3+2;
    }
    // bond data
    Epetra_Vector& bondDamage = *(state.getData(Field_NS::BOND_DAMAGE));
    for(int i=0 ; i<bondDamage.Map().NumMyElements() ; ++i){
      int firstPointInElement = bondDamage.Map().FirstPointInElement(i);
      for(int j=0 ; j<bondDamage.Map().ElementSize(i); ++j)
        bondDamage[firstPointInElement+j] = firstPointInElement+j;
    }
  }

  // check the data
  {
    // scalar data
    Epetra_Vector& ids = *(state.getData(Field_NS::ID));
    for(int i=0 ; i<ids.MyLength() ; ++i)
      BOOST_CHECK_CLOSE(ids[i], i, 1.0e-14);
    // vector data
    Epetra_Vector& force = *(state.getData(Field_NS::FORCE3D));
    for(int i=0 ; i<force.Map().NumMyElements() ; ++i){
      BOOST_CHECK_CLOSE(force[i*3], i*3, 1.0e-14);
      BOOST_CHECK_CLOSE(force[i*3+1], i*3+1, 1.0e-14);
      BOOST_CHECK_CLOSE(force[i*3+2], i*3+2, 1.0e-14);
    }
    // bond data
    Epetra_Vector& bondDamage = *(state.getData(Field_NS::BOND_DAMAGE));
    for(int i=0 ; i<bondDamage.Map().NumMyElements() ; ++i){
      int firstPointInElement = bondDamage.Map().FirstPointInElement(i);
      for(int j=0 ; j<bondDamage.Map().ElementSize(i); ++j)
        BOOST_CHECK_CLOSE(bondDamage[firstPointInElement+j], firstPointInElement+j, 1.0e-14);
    }
  }
}

//! Create a State object for a three-point problem, check data storage and basic functionality.
void threePointProblem()
{
  Teuchos::RCP<Epetra_Comm> comm;
  #ifdef HAVE_MPI
    comm = rcp(new Epetra_MpiComm(MPI_COMM_WORLD));
  #else
    comm = rcp(new Epetra_SerialComm);
  #endif
  int numProcs = comm->NumProc();
  int myPID = comm->MyPID();

  // set up a hard-coded layout for three points
  int numCells = 3;

  // set up overlap maps, which include ghosted nodes
  int numGlobalElements(numCells), numMyElements, elementSize, indexBase(0);
  std::vector<int> myGlobalElements;
  if(numProcs == 1){
    numMyElements = 3;
  }
  else if(numProcs == 2){
    if(myPID == 0)
      numMyElements = 1;
    else if(myPID == 1)
      numMyElements = 2;
  }
  myGlobalElements.resize(numMyElements);
  for(int i=0; i<numMyElements ; ++i)
    myGlobalElements[i] = i+myPID;
  elementSize = 1;

  // oneDimensionalOverlapMap
  // used for cell volumes and scalar constitutive data
  Teuchos::RCP<Epetra_BlockMap> oneDimensionalOverlapMap =
    Teuchos::rcp(new Epetra_BlockMap(numGlobalElements, numMyElements, &myGlobalElements[0], elementSize, indexBase, *comm));
  // threeDimensionalOverlapMap
  // used for positions, displacements, velocities and vector constitutive data
  elementSize = 3;
  Teuchos::RCP<Epetra_BlockMap> threeDimensionalOverlapMap = 
    Teuchos::rcp(new Epetra_BlockMap(numGlobalElements, numMyElements, &myGlobalElements[0], elementSize, indexBase, *comm)); 
  // bondMap
  // used for bond damage and bond constitutive data
  std::vector<int> bondElementSize(numMyElements);
  if(numProcs == 1){
    bondElementSize[0] = 1;
    bondElementSize[1] = 2;
    bondElementSize[2] = 1;
  }
  else if(numProcs == 2){
    if(myPID == 0){
      bondElementSize[0] = 1;
    }
    else if(myPID == 1){
      bondElementSize[0] = 2;
      bondElementSize[1] = 1;
    }
  }
  Teuchos::RCP<Epetra_BlockMap> bondMap = 
    Teuchos::rcp(new Epetra_BlockMap(numGlobalElements, numMyElements, &myGlobalElements[0], &bondElementSize[0], indexBase, *comm));

  // create a state object
  State state;

  // create a list of scalar field specs and allocate the data
  Teuchos::RCP< std::vector<Field_NS::FieldSpec> > scalarFieldSpecs = Teuchos::rcp(new std::vector<Field_NS::FieldSpec>);
  scalarFieldSpecs->push_back(Field_NS::DEFAULT_FIELDTYPE);
  scalarFieldSpecs->push_back(Field_NS::VOLUME);
  scalarFieldSpecs->push_back(Field_NS::ID);
  scalarFieldSpecs->push_back(Field_NS::PROC_NUM);
  scalarFieldSpecs->push_back(Field_NS::DAMAGE);
  scalarFieldSpecs->push_back(Field_NS::WEIGHTED_VOLUME);
  scalarFieldSpecs->push_back(Field_NS::DILATATION);
  scalarFieldSpecs->push_back(Field_NS::NUM_NEIGHBORS);
  scalarFieldSpecs->push_back(Field_NS::LAMBDA);
  scalarFieldSpecs->push_back(Field_NS::SHEAR_CORRECTION_FACTOR);
  state.allocateScalarData(scalarFieldSpecs, oneDimensionalOverlapMap);
  BOOST_CHECK_EQUAL( state.getScalarMultiVector()->NumVectors(), (int)scalarFieldSpecs->size() );
  BOOST_CHECK_EQUAL( state.getScalarMultiVector()->MyLength(), oneDimensionalOverlapMap->NumMyPoints() );
  BOOST_CHECK( state.getScalarMultiVector()->Map().SameAs( *oneDimensionalOverlapMap ) );

  // create a list of vector field specs and allocate the data
  Teuchos::RCP< std::vector<Field_NS::FieldSpec> > vectorFieldSpecs = Teuchos::rcp(new std::vector<Field_NS::FieldSpec>);
  vectorFieldSpecs->push_back(Field_NS::COORD3D);
  vectorFieldSpecs->push_back(Field_NS::DISPL3D);
  vectorFieldSpecs->push_back(Field_NS::CURCOORD3D);
  vectorFieldSpecs->push_back(Field_NS::VELOC3D);
  vectorFieldSpecs->push_back(Field_NS::ACCEL3D);
  vectorFieldSpecs->push_back(Field_NS::FORCE3D);
  vectorFieldSpecs->push_back(Field_NS::FORCE_DENSITY3D);
  vectorFieldSpecs->push_back(Field_NS::CONTACT_FORCE3D);
  vectorFieldSpecs->push_back(Field_NS::CONTACT_FORCE_DENSITY3D);
  state.allocateVectorData(vectorFieldSpecs, threeDimensionalOverlapMap);
  BOOST_CHECK_EQUAL( state.getVectorMultiVector()->NumVectors(), (int)vectorFieldSpecs->size() );
  BOOST_CHECK_EQUAL( state.getVectorMultiVector()->MyLength(), threeDimensionalOverlapMap->NumMyPoints() );
  BOOST_CHECK( state.getVectorMultiVector()->Map().SameAs( *threeDimensionalOverlapMap ) );

  // create a list of bond field specs and allocate the data
  Teuchos::RCP< std::vector<Field_NS::FieldSpec> > bondFieldSpecs = Teuchos::rcp(new std::vector<Field_NS::FieldSpec>);
  bondFieldSpecs->push_back(Field_NS::BOND_DAMAGE);
  bondFieldSpecs->push_back(Field_NS::DEVIATORIC_PLASTIC_EXTENSION);
  bondFieldSpecs->push_back(Field_NS::DEVIATORIC_BACK_EXTENSION);
  state.allocateBondData(bondFieldSpecs, bondMap);
  BOOST_CHECK_EQUAL( state.getBondMultiVector()->NumVectors(), (int)bondFieldSpecs->size() );
  BOOST_CHECK_EQUAL( state.getBondMultiVector()->MyLength(), bondMap->NumMyPoints() );
  BOOST_CHECK( state.getBondMultiVector()->Map().SameAs( *bondMap ) );

  // check initialization of data to zero
  Teuchos::RCP<Epetra_Vector> coordinates = state.getData(Field_NS::COORD3D);
  for(int i=0 ; i<coordinates->MyLength() ; ++i)
    BOOST_CHECK_EQUAL( (*coordinates)[i], 0.0 );

  // set some data
  {
    // scalar data
    Epetra_Vector& ids = *(state.getData(Field_NS::ID));
    for(int i=0 ; i<ids.MyLength() ; ++i)
      ids[i] = i;
    // vector data
    Epetra_Vector& force = *(state.getData(Field_NS::FORCE3D));
    for(int i=0 ; i<force.Map().NumMyElements() ; ++i){
      force[i*3] = i*3;
      force[i*3+1] = i*3+1;
      force[i*3+2] = i*3+2;
    }
    // bond data
    Epetra_Vector& bondDamage = *(state.getData(Field_NS::BOND_DAMAGE));
    for(int i=0 ; i<bondDamage.Map().NumMyElements() ; ++i){
      int firstPointInElement = bondDamage.Map().FirstPointInElement(i);
      for(int j=0 ; j<bondDamage.Map().ElementSize(i); ++j)
        bondDamage[firstPointInElement+j] = firstPointInElement+j;
    }
  }

  // check the data
  {
    // scalar data
    Epetra_Vector& ids = *(state.getData(Field_NS::ID));
    for(int i=0 ; i<ids.MyLength() ; ++i)
      BOOST_CHECK_CLOSE(ids[i], i, 1.0e-14);
    // vector data
    Epetra_Vector& force = *(state.getData(Field_NS::FORCE3D));
    for(int i=0 ; i<force.Map().NumMyElements() ; ++i){
      BOOST_CHECK_CLOSE(force[i*3], i*3, 1.0e-14);
      BOOST_CHECK_CLOSE(force[i*3+1], i*3+1, 1.0e-14);
      BOOST_CHECK_CLOSE(force[i*3+2], i*3+2, 1.0e-14);
    }
    // bond data
    Epetra_Vector& bondDamage = *(state.getData(Field_NS::BOND_DAMAGE));
    for(int i=0 ; i<bondDamage.Map().NumMyElements() ; ++i){
      int firstPointInElement = bondDamage.Map().FirstPointInElement(i);
      for(int j=0 ; j<bondDamage.Map().ElementSize(i); ++j)
        BOOST_CHECK_CLOSE(bondDamage[firstPointInElement+j], firstPointInElement+j, 1.0e-14);
    }
  }
}

//! Test ability to copy data from one State to another.
void copyTo()
{
}

bool init_unit_test_suite()
{
	// Add a suite for each processor in the test
	bool success = true;

	test_suite* proc = BOOST_TEST_SUITE("utPeridigm_State");
	proc->add(BOOST_TEST_CASE(&twoPointProblem));
	proc->add(BOOST_TEST_CASE(&threePointProblem));
	proc->add(BOOST_TEST_CASE(&copyTo));
	framework::master_test_suite().add(proc);

	return success;
}

bool init_unit_test()
{
	init_unit_test_suite();
	return true;
}

int main
(int argc, char* argv[])
{
  int numProcs = 1;
#ifdef HAVE_MPI
  MPI_Init(&argc,&argv);
  MPI_Comm_size(MPI_COMM_WORLD, &numProcs);
#endif

  int returnCode = -1;
  
  if(numProcs == 1 || numProcs == 2){
    returnCode = unit_test_main(init_unit_test, argc, argv);
  }
  else{
    std::cerr << "Unit test runtime ERROR: utPeridigm_State only makes sense on 1 or 2 processors." << std::endl;
  }
  
#ifdef HAVE_MPI
  MPI_Finalize();
#endif
  
  return returnCode;
}