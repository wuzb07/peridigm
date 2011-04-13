/*! \file Peridigm.cpp
 *
 * File containing main class for Peridigm: A parallel, multi-physics,
 * peridynamics simulation code.
 */

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

#include <iostream>
#include <vector>
#include <map>

#include <Epetra_Import.h>
#include <Epetra_LinearProblem.h>
#include <AztecOO.h>
#include <Teuchos_VerboseObject.hpp>

#include "Peridigm.hpp"
#include "Peridigm_DiscretizationFactory.hpp"
#include "Peridigm_OutputManager_VTK_XML.hpp"
#include "Peridigm_ComputeManager.hpp"
#include "contact/Peridigm_ContactModel.hpp"
#include "contact/Peridigm_ShortRangeForceContactModel.hpp"
#include "materials/Peridigm_LinearElasticIsotropicMaterial.hpp"
#include "materials/Peridigm_IsotropicElasticPlasticMaterial.hpp"
#include "PdQuickGrid.h"
#include "PdZoltan.h"
#include "InitialCondition.hpp"
#include "Peridigm_Timer.hpp"

using namespace std;

PeridigmNS::Peridigm::Peridigm(const Teuchos::RCP<const Epetra_Comm>& comm,
                   const Teuchos::RCP<Teuchos::ParameterList>& params)
  : analysisHasRebalance(false),
    rebalanceFrequency(1),
    analysisHasContact(false),
    contactRebalanceFrequency(0),
    contactSearchRadius(0.0)
{
  peridigmComm = comm;
  peridigmParams = params;

  out = Teuchos::VerboseObjectBase::getDefaultOStream();

  // Instantiate materials using provided parameters
  instantiateMaterials();

  // Read mesh from disk or generate using geometric primatives.
  // All maps are generated here
  Teuchos::RCP<Teuchos::ParameterList> discParams = Teuchos::rcp(&(peridigmParams->sublist("Problem").sublist("Discretization")), false);
  DiscretizationFactory discFactory(discParams);
  Teuchos::RCP<AbstractDiscretization> peridigmDisc = discFactory.create(peridigmComm);
  initializeDiscretization(peridigmDisc);

  // Initialize compute manager
  initializeComputeManager();

  // Initialize data manager
  initializeDataManager(peridigmDisc);

  // apply initial velocities
  applyInitialVelocities();

  // Setup contact
  initializeContact();

  // Initialize the workset
  initializeWorkset();

  // Create the model evaluator
  modelEvaluator = Teuchos::rcp(new PeridigmNS::ModelEvaluator(materialModels, contactModels, comm));

  // Initialize material models
  initializeMaterials();

  // Initialize output manager
  initializeOutputManager();

  // Call rebalance function if analysis has contact
  // this is required to set up proper contact neighbor list
  if(analysisHasContact)
    rebalance();
}

void PeridigmNS::Peridigm::instantiateMaterials() {

  // Extract problem parameters sublist
  Teuchos::RCP<Teuchos::ParameterList> problemParams = Teuchos::rcp(&(peridigmParams->sublist("Problem")),false);

  materialModels = Teuchos::rcp(new std::vector< Teuchos::RCP<const PeridigmNS::Material> >()); 

  // Instantiate material objects
  //! \todo Move creation of material models to material model factory
  TEST_FOR_EXCEPT_MSG(!problemParams->isSublist("Material"), "Material parameters not specified!");
  Teuchos::ParameterList & materialParams = problemParams->sublist("Material");
  Teuchos::ParameterList::ConstIterator it;
  for(it = materialParams.begin() ; it != materialParams.end() ; it++){
    const string & name = it->first;
    Teuchos::ParameterList & matParams = materialParams.sublist(name);
    // Insert solver timestep into matParams. Some material models (e.g., viscoelastic) need to know timestep
    Teuchos::RCP<Teuchos::ParameterList> solverParams = Teuchos::rcp(&(peridigmParams->sublist("Solver")),false);
    Teuchos::RCP<Material> material;
    if(name == "Linear Elastic" || name == "Elastic Plastic"){
      if(name == "Linear Elastic")
        material = Teuchos::rcp(new LinearElasticIsotropicMaterial(matParams) );
      else if(name == "Elastic Plastic")
        material = Teuchos::rcp(new IsotropicElasticPlasticMaterial(matParams) );
      materialModels->push_back( Teuchos::rcp_implicit_cast<Material>(material) );
    }
    else {
      string invalidMaterial("Unrecognized material model: ");
      invalidMaterial += name;
      invalidMaterial += ", must be Linear Elastic or Elastic Plastic";
      TEST_FOR_EXCEPT_MSG(true, invalidMaterial);
    }
  }
  TEST_FOR_EXCEPT_MSG(materialModels->size() == 0, "No material models created!");

}

void PeridigmNS::Peridigm::initializeMaterials() {

  std::vector< Teuchos::RCP<const PeridigmNS::Material> >::const_iterator matIt;

  for(matIt = materialModels->begin() ; matIt != materialModels->end() ; matIt++){
    double dt = 0.0;
    (*matIt)->initialize(dt,
                         neighborhoodData->NumOwnedPoints(),
                         neighborhoodData->OwnedIDs(),
                         neighborhoodData->NeighborhoodList(),
                         *dataManager);
  }
}

void PeridigmNS::Peridigm::initializeDiscretization(Teuchos::RCP<AbstractDiscretization> peridigmDisc) {

  // oneDimensionalMap
  // used for cell volumes and scalar constitutive data
  oneDimensionalMap = peridigmDisc->getMap(1); 

  // oneDimensionalOverlapMap
  // used for cell volumes and scalar constitutive data
  // includes ghosts
  oneDimensionalOverlapMap = peridigmDisc->getOverlapMap(1);

  // threeDimensionalMap
  // used for positions, displacements, velocities and vector constitutive data
  threeDimensionalMap = peridigmDisc->getMap(3);

  // threeDimensionalOverlapMap
  // used for positions, displacements, velocities and vector constitutive data
  // includes ghosts
  threeDimensionalOverlapMap = peridigmDisc->getOverlapMap(3);

  // bondConstitutiveDataMap
  // a non-overlapping map used for storing constitutive data on bonds
  bondMap = peridigmDisc->getBondMap();

  // Create mothership vector
  // \todo Do not allocate space for the contact force, residual, and deltaU if not needed.
  mothership = Teuchos::rcp(new Epetra_MultiVector(*threeDimensionalMap, 9));
  // Set ref-count pointers for each of the global vectors
  x = Teuchos::rcp((*mothership)(0), false);             // initial positions
  u = Teuchos::rcp((*mothership)(1), false);             // displacement
  y = Teuchos::rcp((*mothership)(2), false);             // current positions
  v = Teuchos::rcp((*mothership)(3), false);             // velocities
  a = Teuchos::rcp((*mothership)(4), false);             // accelerations
  force = Teuchos::rcp((*mothership)(5), false);         // force
  contactForce = Teuchos::rcp((*mothership)(6), false);  // contact force (used only for contact simulations)
  deltaU = Teuchos::rcp((*mothership)(7), false);        // increment in displacement (used only for implicit time integration)
  residual = Teuchos::rcp((*mothership)(8), false);      // residual (used only for implicit time integration)

  // Set the initial positions
  double* initialX;
  peridigmDisc->getInitialX()->ExtractView(&initialX);
  double* xPtr;
  x->ExtractView(&xPtr);
  blas.COPY(x->MyLength(), initialX, xPtr);
  double* yPtr;
  y->ExtractView(&yPtr);
  blas.COPY(y->MyLength(), initialX, yPtr);

  // Create the importers
  oneDimensionalMapToOneDimensionalOverlapMapImporter = Teuchos::rcp(new Epetra_Import(*oneDimensionalOverlapMap, *oneDimensionalMap));
  threeDimensionalMapToThreeDimensionalOverlapMapImporter = Teuchos::rcp(new Epetra_Import(*threeDimensionalOverlapMap, *threeDimensionalMap));

  // get the neighborlist from the discretization
  neighborhoodData = peridigmDisc->getNeighborhoodData();
}

void PeridigmNS::Peridigm::initializeDataManager(Teuchos::RCP<AbstractDiscretization> peridigmDisc) {

  // Instantiate data manager
  dataManager = Teuchos::rcp(new PeridigmNS::DataManager);
  dataManager->setMaps(oneDimensionalMap, threeDimensionalMap, oneDimensionalOverlapMap, threeDimensionalOverlapMap, bondMap);

  // Create a master list of variable specs
  Teuchos::RCP< std::vector<Field_NS::FieldSpec> > variableSpecs = Teuchos::rcp(new std::vector<Field_NS::FieldSpec>);

  // Start with the specs used by Peridigm
  variableSpecs->push_back(Field_NS::VOLUME);
  variableSpecs->push_back(Field_NS::COORD3D);
  variableSpecs->push_back(Field_NS::DISPL3D);
  variableSpecs->push_back(Field_NS::CURCOORD3D);
  variableSpecs->push_back(Field_NS::VELOC3D);
  variableSpecs->push_back(Field_NS::FORCE_DENSITY3D);
  variableSpecs->push_back(Field_NS::CONTACT_FORCE_DENSITY3D);

  // Add the variable specs requested by each material
  for(unsigned int i=0; i<materialModels->size() ; ++i){
    Teuchos::RCP< std::vector<Field_NS::FieldSpec> > matVariableSpecs = (*materialModels)[i]->VariableSpecs();
    for(unsigned int j=0 ; j<matVariableSpecs->size() ; ++j)
      variableSpecs->push_back((*matVariableSpecs)[j]);
  }

  // Now add the variable specs requested by the compute manager
  std::vector<Field_NS::FieldSpec> computeSpecs = computeManager->getFieldSpecs();
  for (unsigned int i=0; i < computeSpecs.size(); i++) {
     variableSpecs->push_back(computeSpecs[i]);
  }

  // Remove duplicates
  std::unique(variableSpecs->begin(), variableSpecs->end());

  // Allocalte data in the dataManager
  dataManager->allocateData(variableSpecs);

  // Fill the dataManager with data from the discretization
  dataManager->getData(Field_NS::VOLUME, Field_NS::FieldSpec::STEP_NONE)->Import(*(peridigmDisc->getCellVolume()), *oneDimensionalMapToOneDimensionalOverlapMapImporter, Insert);
  dataManager->getData(Field_NS::COORD3D, Field_NS::FieldSpec::STEP_NONE)->Import(*x, *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Insert);
  dataManager->getData(Field_NS::CURCOORD3D, Field_NS::FieldSpec::STEP_N)->Import(*x, *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Insert);
  dataManager->getData(Field_NS::CURCOORD3D, Field_NS::FieldSpec::STEP_NP1)->Import(*x, *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Insert);

}

void PeridigmNS::Peridigm::applyInitialVelocities() {

  TEST_FOR_EXCEPT_MSG(!threeDimensionalMap->SameAs(v->Map()), 
                      "Peridigm::applyInitialVelocities():  Inconsistent velocity vector map.\n");

  /*
   * UNCOMMENT the following two lines
   */

//  RCP<InitialConditionsNS::InitialCondition> icOperator = InitialConditionsNS::getInstance(*peridigmParams);
//  icOperator->apply(*x,*u,*v);

  /*
   * COMMENT OUT ALL OF BELOW TO RUN new Initial Condition Capability
   */
  Teuchos::ParameterList& problemParams = peridigmParams->sublist("Problem");
  Teuchos::ParameterList& bcParams = problemParams.sublist("Boundary Conditions");
  Teuchos::ParameterList::ConstIterator it;

  // get the node sets
  map< string, vector<int> > nodeSets;
  for(it = bcParams.begin() ; it != bcParams.end() ; it++){
	const string& name = it->first;
    // \todo Change input deck so that node sets are parameter lists, not parameters, to avoid this ridiculous search.
	size_t position = name.find("Node Set");
	if(position != string::npos){
	  stringstream ss(Teuchos::getValue<string>(it->second));
	  vector<int> nodeList;
	  int nodeID;
	  while(ss.good()){
		ss >> nodeID;
		nodeList.push_back(nodeID);
	  }
	  nodeSets[name] = nodeList;
	}
  }

  // apply the initial conditions
  for(it = bcParams.begin() ; it != bcParams.end() ; it++){
	const string & name = it->first;
	size_t position = name.find("Initial Velocity");
	if(position != string::npos){
	  Teuchos::ParameterList & boundaryConditionParams = Teuchos::getValue<Teuchos::ParameterList>(it->second);
	  string nodeSet = boundaryConditionParams.get<string>("Node Set");
	  string type = boundaryConditionParams.get<string>("Type");
	  string coordinate = boundaryConditionParams.get<string>("Coordinate");
	  double value = boundaryConditionParams.get<double>("Value");

	  int coord = 0;
	  if(coordinate == "y" || coordinate == "Y")
		coord = 1;
	  if(coordinate == "z" || coordinate == "Z")
		coord = 2;

	  // apply initial velocity boundary conditions
	  // to locally-owned nodes
	  vector<int> & nodeList = nodeSets[nodeSet];
	  for(unsigned int i=0 ; i<nodeList.size() ; i++){
		int localNodeID = threeDimensionalMap->LID(nodeList[i]);
		if(localNodeID != -1)
		  (*v)[localNodeID*3 + coord] = value;
	  }
	}
  }
}

void PeridigmNS::Peridigm::initializeContact() {

  // Extract problem parameters sublist
  Teuchos::RCP<Teuchos::ParameterList> problemParams = Teuchos::rcp(&(peridigmParams->sublist("Problem")),false);

  // Extract discretization parameters sublist
  Teuchos::RCP<Teuchos::ParameterList> discParams = Teuchos::rcp(&(problemParams->sublist("Discretization")), false);

  // Assume no contact
  analysisHasContact = false;
  contactSearchRadius = 0.0;
  contactRebalanceFrequency = 0;

  // Set up contact, if requested by user
  if(problemParams->isSublist("Contact")){
    Teuchos::ParameterList & contactParams = problemParams->sublist("Contact");
    analysisHasContact = true;
    if(!contactParams.isParameter("Search Radius"))
      TEST_FOR_EXCEPTION(true, Teuchos::Exceptions::InvalidParameter, "Contact parameter \"Search Radius\" not specified.");
    contactSearchRadius = contactParams.get<double>("Search Radius");
    if(!contactParams.isParameter("Search Frequency"))
      TEST_FOR_EXCEPTION(true, Teuchos::Exceptions::InvalidParameter, "Contact parameter \"Search Frequency\" not specified.");
    contactRebalanceFrequency = contactParams.get<int>("Search Frequency");
  }

  // Instantiate contact models
  //! \todo Move creation of contact models to contact model factory
  contactModels = Teuchos::rcp(new std::vector<Teuchos::RCP<const PeridigmNS::ContactModel> >);
  if(analysisHasContact){
    Teuchos::ParameterList & contactParams = problemParams->sublist("Contact");
    Teuchos::ParameterList::ConstIterator it;
    for(it = contactParams.begin() ; it != contactParams.end() ; it++){
      const string & name = it->first;
      if(contactParams.isSublist(name)){
        Teuchos::ParameterList & contactModelParams = contactParams.sublist(name);
        // Add the horizon to the contact model parameters, if needed
        if(!contactModelParams.isParameter("Horizon"))
          contactModelParams.set("Horizon", discParams->get<double>("Horizon"));
        Teuchos::RCP<PeridigmNS::ContactModel> contactModel;
        if(name == "Short Range Force"){
          contactModel = Teuchos::rcp(new PeridigmNS::ShortRangeForceContactModel(contactModelParams) );
          contactModels->push_back( Teuchos::rcp_implicit_cast<PeridigmNS::ContactModel>(contactModel) );
        }
        else{
          string invalidContactModel("Unrecognized contact model: ");
          invalidContactModel += name;
          invalidContactModel += ", must be Short Range Force";
          TEST_FOR_EXCEPT_MSG(true, invalidContactModel);
        }
      }
    }
  }
}

void PeridigmNS::Peridigm::initializeWorkset() {
  workset = Teuchos::rcp(new PHAL::Workset);
  Teuchos::RCP<double> timeStep = Teuchos::rcp(new double);
  *timeStep = 0.0;
  workset->timeStep = timeStep;
  workset->dataManager = dataManager;
  workset->jacobian = overlapJacobian;
  workset->materialModels = materialModels;
  workset->neighborhoodData = neighborhoodData;
  workset->contactModels = contactModels;
  workset->contactNeighborhoodData = contactNeighborhoodData;
  workset->myPID = -1;
}

void PeridigmNS::Peridigm::initializeComputeManager() {

  Teuchos::RCP<Teuchos::ParameterList> outputParams;

  if (peridigmParams->isSublist("Output")) {
    outputParams  = Teuchos::rcp(&(peridigmParams->sublist("Output")),false);
  }

  computeManager = Teuchos::rcp( new PeridigmNS::ComputeManager( outputParams  ) );

}


void PeridigmNS::Peridigm::initializeOutputManager() {

  bool active = false;
  Teuchos::RCP<Teuchos::ParameterList> outputParams;

  if (peridigmParams->isSublist("Output")) {
    active = true;
    outputParams  = Teuchos::rcp(&(peridigmParams->sublist("Output")),false);
    outputParams->set("NumProc", (int)(peridigmComm->NumProc()));
    outputParams->set("MyPID", (int)(peridigmComm->MyPID()));
  }

  if (active) {
    // Make the default format "VTK_XML"
    string outputFormat = outputParams->get("Output File Type", "VTK_XML");
    TEST_FOR_EXCEPTION( outputFormat != "VTK_XML",
                        std::invalid_argument,
                        "PeridigmNS::Peridigm: \"Output File Type\" must be \"VTK_XML\".");
    if (outputFormat == "VTK_XML")
       outputManager = Teuchos::rcp(new PeridigmNS::OutputManager_VTK_XML( outputParams ));
    else
      TEST_FOR_EXCEPTION( true, std::invalid_argument,"PeridigmNS::Peridigm::initializeOutputManager: \"Output File Type\" must be \"VTK_XML\".");

    // Query material models for their force state data descriptions
    forceStateDesc = Teuchos::rcp( new Teuchos::ParameterList() );
    for(unsigned int i=0; i<materialModels->size(); ++i){
      Teuchos::ParameterList& subList = forceStateDesc->sublist((*materialModels)[i]->Name());
      Teuchos::RCP< std::vector<Field_NS::FieldSpec> > matVariableSpecs = (*materialModels)[i]->VariableSpecs();
      for(unsigned int j=0 ; j<matVariableSpecs->size() ; ++j)
        subList.set( (*matVariableSpecs)[j].getLabel(), j);
    }

    // Initialize current time in this parameterlist
    Teuchos::RCP<Teuchos::ParameterList> solverParams = Teuchos::rcp(&(peridigmParams->sublist("Solver")),false);
    double t_initial = solverParams->get("Initial Time", 0.0);
    forceStateDesc->set<double>("Time", t_initial);
    // Set RCP to neighborlist
    forceStateDesc->set("Bond Family",neighborhoodData);
    // Ask OutputManager to write initial conditions to disk
    outputManager->write(x,u,v,a,force,dataManager,neighborhoodData,forceStateDesc);
//    this->synchDataManager();
//    outputManager->write(dataManager,neighborhoodData,forceStateDesc);
  }
  else { // no output requested
    outputManager = Teuchos::rcp(new PeridigmNS::OutputManager_VTK_XML( outputParams ));
  }

  //  verbose = problemParams->get("Verbose", false);

}

void PeridigmNS::Peridigm::execute() {

  Teuchos::RCP<Teuchos::ParameterList> solverParams = sublist(peridigmParams, "Solver", true);

  // allowable explicit time integration schemes:  Verlet
  if(solverParams->isSublist("Verlet"))
    executeExplicit();

  // allowable implicit time integration schemes:  Implicit
  else if(solverParams->isSublist("Implicit"))    
    executeImplicit();

}

void PeridigmNS::Peridigm::executeExplicit() {

  Teuchos::RCP<double> timeStep = Teuchos::rcp(new double);
  workset->timeStep = timeStep;

  // Copy data from mothership vectors to overlap vectors in data manager
  dataManager->getData(Field_NS::DISPL3D, Field_NS::FieldSpec::STEP_NP1)->Import(*u, *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Insert);
  dataManager->getData(Field_NS::CURCOORD3D, Field_NS::FieldSpec::STEP_NP1)->Import(*y, *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Insert);
  dataManager->getData(Field_NS::VELOC3D, Field_NS::FieldSpec::STEP_NP1)->Import(*v, *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Insert);

  // evalModel() should be called by time integrator here...
  // For now, insert Verlet intergrator here
  Teuchos::RCP<Teuchos::ParameterList> solverParams = sublist(peridigmParams, "Solver", true);
  Teuchos::RCP<Teuchos::ParameterList> verletParams = sublist(solverParams, "Verlet", true);
  double t_initial = solverParams->get("Initial Time", 0.0);
  double t_final   = solverParams->get("Final Time", 1.0);
  double t_current = t_initial;
  double dt        = verletParams->get("Fixed dt", 1.0);
  *timeStep = dt;
  double dt2 = dt/2.0;
  int nsteps = (int)floor((t_final-t_initial)/dt);
  if(solverParams->isSublist("Rebalance")){
    Teuchos::RCP<Teuchos::ParameterList> rebalanceParams = sublist(solverParams, "Rebalance", true);
    analysisHasRebalance = true;
    rebalanceFrequency = rebalanceParams->get("Rebalance Frequency", 1);
  }
  // Pointer index into sub-vectors for use with BLAS
  double *xptr, *uptr, *yptr, *vptr, *aptr;
  x->ExtractView( &xptr );
  u->ExtractView( &uptr );
  y->ExtractView( &yptr );
  v->ExtractView( &vptr );
  a->ExtractView( &aptr );
  int length = a->MyLength();

  for(int step=1; step<=nsteps ; step++){

    // rebalance, if requested
    if( (analysisHasRebalance && step%rebalanceFrequency == 0) || (analysisHasContact && step%contactRebalanceFrequency == 0) ){
      PeridigmNS::Timer::self().startTimer("Rebalance");
      rebalance();
      PeridigmNS::Timer::self().stopTimer("Rebalance");
      x->ExtractView( &xptr );
      u->ExtractView( &uptr );
      y->ExtractView( &yptr );
      v->ExtractView( &vptr );
      a->ExtractView( &aptr );
      length = a->MyLength();
    }

    // Do one step of velocity-Verlet

    // \todo This acceleration will be zero at time step 1, and that is not correct in general, need a bootstrap step prior to integration loop.

    // V^{n+1/2} = V^{n} + (dt/2)*A^{n}
    //blas.AXPY(const int N, const double ALPHA, const double *X, double *Y, const int INCX=1, const int INCY=1) const
    blas.AXPY(length, dt2, aptr, vptr, 1, 1);

    // Y^{n+1} = X_{o} + U^{n} + (dt)*V^{n+1/2}
    // \todo Replace with blas call
    for(int i=0 ; i<y->MyLength() ; ++i)
      yptr[i] = xptr[i] + uptr[i] + dt*vptr[i];

    // U^{n+1} = U^{n} + (dt)*V^{n+1/2}
    //blas.AXPY(const int N, const double ALPHA, const double *X, double *Y, const int INCX=1, const int INCY=1) const
    blas.AXPY(length, dt, vptr, uptr, 1, 1);

    // \todo The velocity copied into the DataManager is actually the midstep velocity, not the NP1 velocity; this can be fixed by creating a midstep velocity field in the DataManager and setting the NP1 value as invalid.

    // Copy data from mothership vectors to overlap vectors in data manager
    PeridigmNS::Timer::self().startTimer("Gather/Scatter");
    dataManager->getData(Field_NS::DISPL3D, Field_NS::FieldSpec::STEP_NP1)->Import(*u, *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Insert);
    dataManager->getData(Field_NS::CURCOORD3D, Field_NS::FieldSpec::STEP_NP1)->Import(*y, *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Insert);
    dataManager->getData(Field_NS::VELOC3D, Field_NS::FieldSpec::STEP_NP1)->Import(*v, *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Insert);
    PeridigmNS::Timer::self().stopTimer("Gather/Scatter");

    // Update forces based on new positions
    PeridigmNS::Timer::self().startTimer("Model Evaluator");
    modelEvaluator->evalModel(workset);
    PeridigmNS::Timer::self().stopTimer("Model Evaluator");

    // Copy force from the data manager to the mothership vector
    PeridigmNS::Timer::self().startTimer("Gather/Scatter");
    force->Export(*dataManager->getData(Field_NS::FORCE_DENSITY3D, Field_NS::FieldSpec::STEP_NP1), *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Add);
    PeridigmNS::Timer::self().stopTimer("Gather/Scatter");    

    if(analysisHasContact){
      // Copy contact force from the data manager to the mothership vector
      PeridigmNS::Timer::self().startTimer("Gather/Scatter");
      contactForce->Export(*dataManager->getData(Field_NS::CONTACT_FORCE_DENSITY3D, Field_NS::FieldSpec::STEP_NP1), *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Add);
      PeridigmNS::Timer::self().stopTimer("Gather/Scatter");

      // Add contact forces to forces
      force->Update(1.0, *contactForce, 1.0);
    }

    // fill the acceleration vector
    (*a) = (*force);
    // \todo Possibly move this functionality into ModelEvaluator.
    // \todo Generalize this for multiple materials
    double density = (*materialModels)[0]->Density();
    a->Scale(1.0/density);

    // V^{n+1}   = V^{n+1/2} + (dt/2)*A^{n+1}
    //blas.AXPY(const int N, const double ALPHA, const double *X, double *Y, const int INCX=1, const int INCY=1) const
    blas.AXPY(length, dt2, aptr, vptr, 1, 1);

    t_current = t_initial + (step*dt);
    forceStateDesc->set("Time", t_current);

    PeridigmNS::Timer::self().startTimer("Output");
    outputManager->write(x,u,v,a,force,dataManager,neighborhoodData,forceStateDesc);
//    this->synchDataManager();
//    outputManager->write(dataManager,neighborhoodData,forceStateDesc);
    PeridigmNS::Timer::self().stopTimer("Output");

    // swap state N and state NP1
    dataManager->updateState();
  }
}

void PeridigmNS::Peridigm::executeImplicit() {

  // Allocate memory for non-zeros in global Jacobain and lock in the structure
  allocateJacobian();

  Teuchos::RCP<double> timeStep = Teuchos::rcp(new double);
  workset->timeStep = timeStep;

  Teuchos::RCP<Teuchos::ParameterList> solverParams = sublist(peridigmParams, "Solver", true);
  Teuchos::RCP<Teuchos::ParameterList> implicitParams = sublist(solverParams, "Implicit", true);
  double timeInitial = solverParams->get("Initial Time", 0.0);
  double timeFinal = solverParams->get("Final Time", 1.0);
  double timeCurrent = timeInitial;
  int numLoadSteps = implicitParams->get("Number of Load Steps", 10);
  double absoluteTolerance = implicitParams->get("Absolute Tolerance", 1.0e-6);
  double maximumSolverIterations = implicitParams->get("Maximum Solver Iterations", 10);

  // Pointer index into sub-vectors for use with BLAS
  double *xptr, *uptr, *yptr, *vptr, *aptr;
  x->ExtractView( &xptr );
  u->ExtractView( &uptr );
  y->ExtractView( &yptr );
  v->ExtractView( &vptr );
  a->ExtractView( &aptr );

  // \todo Put in mothership.
  Epetra_Vector lhs(*residual);

  for(int step=0; step<numLoadSteps ; step++){

    double loadIncrement = 1.0/double(numLoadSteps);
    double dt = (timeFinal - timeInitial)*loadIncrement;
    timeCurrent += dt;
    *timeStep = dt;
    
    if(peridigmComm->MyPID() == 0)
      cout << "Load step " << step+1 << ", load increment = " << loadIncrement << ", time step = " << dt << ", current time = " << timeCurrent << endl;

    // Update nodal positions for nodes with kinematic B.C.
    deltaU->PutScalar(0.0);
    applyKinematicBC(loadIncrement, deltaU, Teuchos::RCP<Epetra_FECrsMatrix>());

    // Set the current position
    // \todo We probably want to rework this so that the material models get valid x, u, and y values
    // Currently the u values are from the previous load step (and if we update u here we'll be unable to properly undo a time step, which we'll need to adaptive time stepping).
    for(int i=0 ; i<y->MyLength() ; ++i)
      (*y)[i] = (*x)[i] + (*u)[i] + (*deltaU)[i];

    // compute the residual
    double residualNorm = computeResidual();

    int solverIteration = 1;
    while(residualNorm > absoluteTolerance && solverIteration <= maximumSolverIterations){

      if(peridigmComm->MyPID() == 0)
        cout << "  residual = " << residualNorm << endl;

      // Compute the tangent
      tangent->PutScalar(0.0);
      PeridigmNS::Timer::self().startTimer("Evaluate Jacobian");
      modelEvaluator->evalJacobian(workset);
      tangent->GlobalAssemble();
      PeridigmNS::Timer::self().stopTimer("Evaluate Jacobian");
      applyKinematicBC(0.0, residual, tangent);
      residual->Scale(-1.0);

      // Solver linear system
      PeridigmNS::Timer::self().startTimer("Solve Linear System");
      Epetra_LinearProblem linearProblem;
      AztecOO solver(linearProblem);
      solver.SetAztecOption(AZ_precond, AZ_Jacobi);
      stringstream ss;
      solver.SetOutputStream(ss);
      int maxAztecIterations = 500;
      double aztecTolerance = 1.0e-6;
      lhs.PutScalar(0.0);
      solver.Iterate(tangent.get(), &lhs, residual.get(), maxAztecIterations, aztecTolerance);
      PeridigmNS::Timer::self().stopTimer("Solve Linear System");

      // Apply increment to nodal positions
      for(int i=0 ; i<y->MyLength() ; ++i)
        (*deltaU)[i] += lhs[i];
      for(int i=0 ; i<y->MyLength() ; ++i)
        (*y)[i] = (*x)[i] + (*u)[i] + (*deltaU)[i];
      
      // Compute residual
      residualNorm = computeResidual();

      solverIteration++;
    }

    if(peridigmComm->MyPID() == 0)
      cout << "  residual = " << residualNorm << endl;

    // Add the converged displacement increment to the displacement
    for(int i=0 ; i<u->MyLength() ; ++i)
      (*u)[i] += (*deltaU)[i];

    // Write output for completed load step
    PeridigmNS::Timer::self().startTimer("Output");
    forceStateDesc->set("Time", timeCurrent);
    outputManager->write(x,u,v,a,force,dataManager,neighborhoodData,forceStateDesc);
    PeridigmNS::Timer::self().stopTimer("Output");

    // swap state N and state NP1
    dataManager->updateState();

    cout << endl;
  }
}

void PeridigmNS::Peridigm::allocateJacobian() {

  // Construct map for global tangent matrix
  // Note that this must be an Epetra_Map, not an Epetra_BlockMap, so we can't use threeDimensionalMap directly
  int numGlobalElements = 3*oneDimensionalMap->NumGlobalElements();
  int numMyElements = 3*oneDimensionalMap->NumMyElements();
  int* myGlobalElements = new int[numMyElements];
  int* oneDimensionalMapGlobalElements = oneDimensionalMap->MyGlobalElements();
  for(int iElem=0 ; iElem<oneDimensionalMap->NumMyElements() ; ++iElem){
    myGlobalElements[3*iElem]     = 3*oneDimensionalMapGlobalElements[iElem];
    myGlobalElements[3*iElem + 1] = 3*oneDimensionalMapGlobalElements[iElem] + 1;
    myGlobalElements[3*iElem + 2] = 3*oneDimensionalMapGlobalElements[iElem] + 2;
  }
  int indexBase = 0;
  tangentMap = Teuchos::rcp(new Epetra_Map(numGlobalElements, numMyElements, myGlobalElements, indexBase, *peridigmComm));
  delete[] myGlobalElements;

  // Create the global tangent matrix
  Epetra_DataAccess CV = Copy;
  int numEntriesPerRow = 0;  // If this is zero, allocation will take place during the insertion phase \todo Compute non-zeros instead of allocation during insertion.
  bool staticProfile = false;  // \todo Can staticProfile be set to true?  Bond breaking would alter the non-zeros, but we could just leave them there to avoid reallocation.
  tangent = Teuchos::rcp(new Epetra_FECrsMatrix(CV, *tangentMap, numEntriesPerRow, staticProfile));

  // Loop over the neighborhood for each locally-owned point and create non-zero entries in the matrix
  vector<int> globalIndicies;
  vector<double> zeros;
  int* neighborhoodList = neighborhoodData->NeighborhoodList();
  int neighborhoodListIndex = 0;
  for(int LID=0 ; LID<neighborhoodData->NumOwnedPoints() ; ++LID){
    int GID =  oneDimensionalOverlapMap->GID(LID);
    int numNeighbors = neighborhoodList[neighborhoodListIndex++];
    unsigned int numEntries = 3*(numNeighbors+1);
    globalIndicies.resize(numEntries);
    globalIndicies[0] = 3*GID;
    globalIndicies[1] = 3*GID + 1;
    globalIndicies[2] = 3*GID + 2;
    for(int j=0 ; j<numNeighbors ; ++j){
      int neighborLocalID = neighborhoodList[neighborhoodListIndex++];
      int neighborGlobalID = oneDimensionalOverlapMap->GID(neighborLocalID);
      globalIndicies[3*j+3] = 3*neighborGlobalID;
      globalIndicies[3*j+4] = 3*neighborGlobalID + 1;
      globalIndicies[3*j+5] = 3*neighborGlobalID + 2;
    }
    if(numEntries > zeros.size())
      zeros.resize(numEntries, 0.0);
    tangent->InsertGlobalValues(3*GID,   numEntries, &zeros[0], &globalIndicies[0]); 
    tangent->InsertGlobalValues(3*GID+1, numEntries, &zeros[0], &globalIndicies[0]); 
    tangent->InsertGlobalValues(3*GID+2, numEntries, &zeros[0], &globalIndicies[0]); 
  }
  tangent->GlobalAssemble();

  // create the serial Jacobian
  overlapJacobian = Teuchos::rcp(new PeridigmNS::SerialMatrix(tangent, oneDimensionalOverlapMap));
  workset->jacobian = overlapJacobian;
}

void PeridigmNS::Peridigm::applyKinematicBC(double loadIncrement,
                                            Teuchos::RCP<Epetra_Vector> vec,
                                            Teuchos::RCP<Epetra_FECrsMatrix> mat) {
  PeridigmNS::Timer::self().startTimer("Apply Kinematic B.C.");

  Teuchos::ParameterList& problemParams = peridigmParams->sublist("Problem");
  Teuchos::ParameterList& bcParams = problemParams.sublist("Boundary Conditions");
  Teuchos::ParameterList::ConstIterator it;

  // get the node sets
  map< string, vector<int> > nodeSets;
  for(it = bcParams.begin() ; it != bcParams.end() ; it++){
	const string& name = it->first;
    // \todo Change input deck so that node sets are parameter lists, not parameters, to avoid this ridiculous search.
	size_t position = name.find("Node Set");
	if(position != string::npos){
	  stringstream ss(Teuchos::getValue<string>(it->second));
	  vector<int> nodeList;
	  int nodeID;
	  while(ss.good()){
		ss >> nodeID;
		nodeList.push_back(nodeID);
	  }
	  nodeSets[name] = nodeList;
	}
  }

  // create data structures for inserting ones and zeros into jacobian
  vector<double> jacobianRow;
  vector<int> jacobianIndicies;
  if(!mat.is_null()){
    jacobianRow.resize(mat->NumMyCols(), 0.0);
    jacobianIndicies.resize(mat->NumMyCols());
    for(unsigned int i=0 ; i<jacobianIndicies.size() ; ++i)
      jacobianIndicies[i] = i;
  }

  // apply the kinematic boundary conditions
  for(it = bcParams.begin() ; it != bcParams.end() ; it++){
	const string & name = it->first;
	size_t position = name.find("Prescribed Displacement");
	if(position != string::npos){
	  Teuchos::ParameterList & boundaryConditionParams = Teuchos::getValue<Teuchos::ParameterList>(it->second);
	  string nodeSet = boundaryConditionParams.get<string>("Node Set");
	  string type = boundaryConditionParams.get<string>("Type");
	  string coordinate = boundaryConditionParams.get<string>("Coordinate");
	  double value = boundaryConditionParams.get<double>("Value");

	  int coord = 0;
	  if(coordinate == "y" || coordinate == "Y")
		coord = 1;
	  if(coordinate == "z" || coordinate == "Z")
		coord = 2;

	  // apply kinematic boundary conditions to locally-owned nodes
	  vector<int> & nodeList = nodeSets[nodeSet];
	  for(unsigned int i=0 ; i<nodeList.size() ; i++){

        // zero out the row and column and put a 1.0 on the diagonal
        if(!mat.is_null()){
          int globalID = 3*nodeList[i] + coord;
          int localRowID = mat->LRID(globalID);
          int localColID = mat->LCID(globalID);

          // zero out all locally-owned entries in the column associated with this dof
          // \todo Call ReplaceMyValues only for entries that actually exist in the matrix structure.
          double zero = 0.0;
          for(int iRow=0 ; iRow<mat->NumMyRows() ; ++iRow)
            mat->ReplaceMyValues(iRow, 1, &zero, &localColID);

          // zero out the row and put a 1.0 on the diagonal
          if(localRowID != -1){
            jacobianRow[localColID] = 1.0;
            // From Epetra_CrsMatrix documentation:
            // If a value is not already present for the specified location in the matrix, the
            // input value will be ignored and a positive warning code will be returned.
            // \todo Do the bookkeeping to send in data only for locations that actually exist in the matrix structure.
            mat->ReplaceMyValues(localRowID, mat->NumMyCols(), &jacobianRow[0], &jacobianIndicies[0]);
            jacobianRow[localColID] = 0.0;
          }
        }

        // set entry in residual vector equal to the displacement increment for the kinematic bc
        // this will cause the solution procedure to solve for the correct U at the bc
		int localNodeID = threeDimensionalMap->LID(nodeList[i]);
		if(!vec.is_null() && localNodeID != -1){
 		  (*vec)[localNodeID*3 + coord] = value*loadIncrement;
        }
	  }
	}
  }
  PeridigmNS::Timer::self().stopTimer("Apply Kinematic B.C.");
}

double PeridigmNS::Peridigm::computeResidual() {

  PeridigmNS::Timer::self().startTimer("Compute Residual");

  // The residual is computed as the L2 norm of the internal force vector with the
  // entries corresponding to kinematic BC zeroed out.

  // Copy data from mothership vectors to overlap vectors in data manager
  PeridigmNS::Timer::self().startTimer("Gather/Scatter");
  dataManager->getData(Field_NS::DISPL3D, Field_NS::FieldSpec::STEP_NP1)->Import(*u, *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Insert);
  dataManager->getData(Field_NS::CURCOORD3D, Field_NS::FieldSpec::STEP_NP1)->Import(*y, *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Insert);
  dataManager->getData(Field_NS::VELOC3D, Field_NS::FieldSpec::STEP_NP1)->Import(*v, *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Insert);
  PeridigmNS::Timer::self().stopTimer("Gather/Scatter");

  // Update forces based on new positions
  PeridigmNS::Timer::self().startTimer("Model Evaluator");
  modelEvaluator->evalModel(workset);
  PeridigmNS::Timer::self().stopTimer("Model Evaluator");

  // Copy force from the data manager to the mothership vector
  PeridigmNS::Timer::self().startTimer("Gather/Scatter");
  force->Export(*dataManager->getData(Field_NS::FORCE_DENSITY3D, Field_NS::FieldSpec::STEP_NP1), *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Add);
  PeridigmNS::Timer::self().stopTimer("Gather/Scatter");

  // copy the internal force to the residual vector
  (*residual) = (*force);
    
  // zero out the rows corresponding to kinematic boundary conditions and compute the residual
  applyKinematicBC(0.0, residual, Teuchos::RCP<Epetra_FECrsMatrix>());
  double residualNorm2;
  residual->Norm2(&residualNorm2);

  PeridigmNS::Timer::self().stopTimer("Compute Residual");

  return residualNorm2;
}

void PeridigmNS::Peridigm::synchDataManager() {
  // Need to ensure these primal fields are synchronized: VOLUME, COORD3D, DISPL3D, CURCOORD3D, VELOC3D, FORCE_DENSITY3D, CONTACT_FORCE_DENSITY_3D

  // Copy data from mothership vectors to overlap vectors in data manager
  // VOLUME is synched during creation and rebalance, and otherwise never changes
  // COORD3D is synched during creation and rebalance, and otherwise never changes
  PeridigmNS::Timer::self().startTimer("Gather/Scatter");
  dataManager->getData(Field_NS::DISPL3D, Field_NS::FieldSpec::STEP_NP1)->Import(*u, *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Insert);
  dataManager->getData(Field_NS::CURCOORD3D, Field_NS::FieldSpec::STEP_NP1)->Import(*y, *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Insert);
  dataManager->getData(Field_NS::VELOC3D, Field_NS::FieldSpec::STEP_NP1)->Import(*v, *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Insert);
  dataManager->getData(Field_NS::FORCE_DENSITY3D, Field_NS::FieldSpec::STEP_NP1)->Import(*force, *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Insert);
  if(analysisHasContact){
    dataManager->getData(Field_NS::CONTACT_FORCE_DENSITY3D, Field_NS::FieldSpec::STEP_NP1)->Import(*contactForce, *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Insert);
  }
  PeridigmNS::Timer::self().stopTimer("Gather/Scatter");

}

void PeridigmNS::Peridigm::rebalance() {

  // \todo Handle serial case.  We don't need to rebalance, but we still want to update the contact search.

  PdGridData rebalancedDecomp = currentConfigurationDecomp();

  Teuchos::RCP<Epetra_BlockMap> rebalancedOneDimensionalMap = Teuchos::rcp(new Epetra_BlockMap(PdQuickGrid::getOwnedMap(*peridigmComm, rebalancedDecomp, 1)));
  Teuchos::RCP<const Epetra_Import> oneDimensionalMapImporter = Teuchos::rcp(new Epetra_Import(*rebalancedOneDimensionalMap, *oneDimensionalMap));

  Teuchos::RCP<Epetra_BlockMap> rebalancedThreeDimensionalMap = Teuchos::rcp(new Epetra_BlockMap(PdQuickGrid::getOwnedMap(*peridigmComm, rebalancedDecomp, 3)));
  Teuchos::RCP<const Epetra_Import> threeDimensionalMapImporter = Teuchos::rcp(new Epetra_Import(*rebalancedThreeDimensionalMap, *threeDimensionalMap));

  Teuchos::RCP<Epetra_BlockMap> rebalancedBondMap = createRebalancedBondMap(rebalancedOneDimensionalMap, oneDimensionalMapImporter);
  Teuchos::RCP<const Epetra_Import> bondMapImporter = Teuchos::rcp(new Epetra_Import(*rebalancedBondMap, *bondMap));

  // create a list of neighbors in the rebalanced configuration
  // this list has the global ID for each neighbor of each on-processor point (that is, on-processor in the rebalanced configuration)
  Teuchos::RCP<Epetra_Vector> rebalancedNeighborGlobalIDs = createRebalancedNeighborGlobalIDList(rebalancedBondMap, bondMapImporter);

  // create a list of all the off-processor IDs that will need to be ghosted
  // \todo Use set::reserve() for better memory allocation here.
  set<int> offProcessorIDs;
  for(int i=0 ; i<rebalancedNeighborGlobalIDs->MyLength() ; ++i){
    int globalID = (int)( (*rebalancedNeighborGlobalIDs)[i] );
    if(!rebalancedOneDimensionalMap->MyGID(globalID))
      offProcessorIDs.insert(globalID);
  }

  // this function does three things:
  // 1) fills the neighborhood information in rebalancedDecomp based on the contact search
  // 2) creates a list of global IDs for each locally-owned point that will need to be searched for contact (contactNeighborGlobalIDs)
  // 3) keeps track of the additional off-processor IDs that need to be ghosted as a result of the contact search (offProcessorContactIDs)
  Teuchos::RCP< map<int, vector<int> > > contactNeighborGlobalIDs = Teuchos::rcp(new map<int, vector<int> >());
  Teuchos::RCP< set<int> > offProcessorContactIDs = Teuchos::rcp(new set<int>());
  if(analysisHasContact)
    contactSearch(rebalancedOneDimensionalMap, rebalancedBondMap, rebalancedNeighborGlobalIDs, rebalancedDecomp, contactNeighborGlobalIDs, offProcessorContactIDs);

  // add the off-processor IDs required for contact to the list of points that will be ghosted
  for(set<int>::const_iterator it=offProcessorContactIDs->begin() ; it!=offProcessorContactIDs->end() ; it++){
    offProcessorIDs.insert(*it);
  }

  // construct the rebalanced overlap maps
  int numGlobalElements = -1;
  int numMyElements = rebalancedOneDimensionalMap->NumMyElements() + offProcessorIDs.size();
  int* myGlobalElements = new int[numMyElements];
  rebalancedOneDimensionalMap->MyGlobalElements(myGlobalElements);
  int offset = rebalancedOneDimensionalMap->NumMyElements();
  int index = 0;
  for(set<int>::const_iterator it=offProcessorIDs.begin() ; it!=offProcessorIDs.end() ; ++it, ++index){
    myGlobalElements[offset+index] = *it;
  }
  int indexBase = 0;
  Teuchos::RCP<Epetra_BlockMap> rebalancedOneDimensionalOverlapMap =
    Teuchos::rcp(new Epetra_BlockMap(numGlobalElements, numMyElements, myGlobalElements, 1, indexBase, *peridigmComm));
  Teuchos::RCP<Epetra_BlockMap> rebalancedThreeDimensionalOverlapMap =
    Teuchos::rcp(new Epetra_BlockMap(numGlobalElements, numMyElements, myGlobalElements, 3, indexBase, *peridigmComm));
  delete[] myGlobalElements;

  Teuchos::RCP<const Epetra_Import> oneDimensionalOverlapMapImporter = Teuchos::rcp(new Epetra_Import(*rebalancedOneDimensionalOverlapMap, *oneDimensionalOverlapMap));
  Teuchos::RCP<const Epetra_Import> threeDimensionalOverlapMapImporter = Teuchos::rcp(new Epetra_Import(*rebalancedThreeDimensionalOverlapMap, *threeDimensionalOverlapMap));

  // create a new NeighborhoodData object
  Teuchos::RCP<PeridigmNS::NeighborhoodData> rebalancedNeighborhoodData = createRebalancedNeighborhoodData(rebalancedOneDimensionalMap,
                                                                                                           rebalancedOneDimensionalOverlapMap,
                                                                                                           rebalancedBondMap,
                                                                                                           rebalancedNeighborGlobalIDs);

  // create a new NeighborhoodData object for contact
  Teuchos::RCP<PeridigmNS::NeighborhoodData> rebalancedContactNeighborhoodData;
  if(analysisHasContact)
    rebalancedContactNeighborhoodData = createRebalancedContactNeighborhoodData(contactNeighborGlobalIDs,
                                                                                rebalancedOneDimensionalMap,
                                                                                rebalancedOneDimensionalOverlapMap);

  // rebalance the global vectors (stored in the mothership multivector)
  Teuchos::RCP<Epetra_MultiVector> rebalancedMothership = Teuchos::rcp(new Epetra_MultiVector(*rebalancedThreeDimensionalMap, mothership->NumVectors()));
  rebalancedMothership->Import(*mothership, *threeDimensionalMapImporter, Insert);
  mothership = rebalancedMothership;
  x = Teuchos::rcp((*mothership)(0), false);
  u = Teuchos::rcp((*mothership)(1), false);
  y = Teuchos::rcp((*mothership)(2), false);
  v = Teuchos::rcp((*mothership)(3), false);
  a = Teuchos::rcp((*mothership)(4), false);
  force = Teuchos::rcp((*mothership)(5), false);
  contactForce = Teuchos::rcp((*mothership)(6), false);

  // rebalance the data manager
  dataManager->rebalance(rebalancedOneDimensionalMap,
                         rebalancedThreeDimensionalMap,
                         rebalancedOneDimensionalOverlapMap,
                         rebalancedThreeDimensionalOverlapMap,
                         rebalancedBondMap);

  // set all the pointers to the new maps
  oneDimensionalMap = rebalancedOneDimensionalMap;
  oneDimensionalOverlapMap = rebalancedOneDimensionalOverlapMap;
  threeDimensionalMap = rebalancedThreeDimensionalMap;
  threeDimensionalOverlapMap = rebalancedThreeDimensionalOverlapMap;
  bondMap = rebalancedBondMap;

  // update neighborhood data
  neighborhoodData = rebalancedNeighborhoodData;
  workset->neighborhoodData = neighborhoodData; // \todo Better handling of workset, shouldn't have to do this here.
  contactNeighborhoodData = rebalancedContactNeighborhoodData;
  workset->contactNeighborhoodData = contactNeighborhoodData;

  // update importers
  oneDimensionalMapToOneDimensionalOverlapMapImporter = Teuchos::rcp(new Epetra_Import(*oneDimensionalOverlapMap, *oneDimensionalMap));
  threeDimensionalMapToThreeDimensionalOverlapMapImporter = Teuchos::rcp(new Epetra_Import(*threeDimensionalOverlapMap, *threeDimensionalMap));
}

PdGridData PeridigmNS::Peridigm::currentConfigurationDecomp() {

  // Create a decomp object and fill necessary data for rebalance
  int myNumElements = oneDimensionalMap->NumMyElements();
  int dimension = 3;
  PdGridData decomp = PdQuickGrid::allocatePdGridData(myNumElements, dimension);

  decomp.globalNumPoints = oneDimensionalMap->NumGlobalElements();

  // \todo myGlobalIDs, cellVolume, and myX are allocated in allocatePdGridData(), don't need to allocate here.
  
  // fill myGlobalIDs
  shared_ptr<int> myGlobalIDs(new int[myNumElements], PdQuickGrid::Deleter<int>());
  int* myGlobalIDsPtr = myGlobalIDs.get();
  int* gIDs = oneDimensionalMap->MyGlobalElements();
  memcpy(myGlobalIDsPtr, gIDs, myNumElements*sizeof(int));
  decomp.myGlobalIDs = myGlobalIDs;

  // fill myX and cellVolume
  // use current positions for x
  shared_ptr<double> myX(new double[myNumElements*dimension], PdQuickGrid::Deleter<double>());
  double* myXPtr = myX.get();
  double* yPtr;
  y->ExtractView(&yPtr);
  memcpy(myXPtr, yPtr, myNumElements*dimension*sizeof(double));
  shared_ptr<double> cellVolume(new double[myNumElements], PdQuickGrid::Deleter<double>());
  double* cellVolumePtr = cellVolume.get();
  double* cellVolumeOverlapPtr;
  dataManager->getData(Field_NS::VOLUME, Field_NS::FieldSpec::STEP_NONE)->ExtractView(&cellVolumeOverlapPtr);
  for(int i=0 ; i<myNumElements ; ++i){
    int oneDimensionalMapGlobalID = oneDimensionalMap->GID(i);
    int oneDimensionalOverlapMapLocalID = oneDimensionalOverlapMap->LID(oneDimensionalMapGlobalID);
    cellVolumePtr[i] = cellVolumeOverlapPtr[oneDimensionalOverlapMapLocalID];
  }  
  decomp.myX = myX;
  decomp.cellVolume = cellVolume;

  // call the rebalance function on the current-configuration decomp
  decomp = getLoadBalancedDiscretization(decomp);

  return decomp;
}

Teuchos::RCP<Epetra_BlockMap> PeridigmNS::Peridigm::createRebalancedBondMap(Teuchos::RCP<Epetra_BlockMap> rebalancedOneDimensionalMap,
                                                                            Teuchos::RCP<const Epetra_Import> oneDimensionalMapToRebalancedOneDimensionalMapImporter) {

  // communicate the number of bonds for each point so that space for bond data can be allocated
  Teuchos::RCP<Epetra_Vector> numberOfBonds = Teuchos::rcp(new Epetra_Vector(*oneDimensionalMap));
  for(int i=0 ; i<oneDimensionalMap->NumMyElements() ; ++i){
    int globalID = oneDimensionalMap->GID(i);
    int bondMapLocalID = bondMap->LID(globalID);
    if(bondMapLocalID != -1)
      (*numberOfBonds)[i] = (double)( bondMap->ElementSize(i) );
  }
  Teuchos::RCP<Epetra_Vector> rebalancedNumberOfBonds = Teuchos::rcp(new Epetra_Vector(*rebalancedOneDimensionalMap));
  rebalancedNumberOfBonds->Import(*numberOfBonds, *oneDimensionalMapToRebalancedOneDimensionalMapImporter, Insert);

  // create the rebalanced bond map
  // care must be taken because you cannot have an element with zero length
  int numMyElementsUpperBound = rebalancedOneDimensionalMap->NumMyElements();
  int numGlobalElements = -1; 
  int numMyElements = 0;
  int* rebalancedOneDimensionalMapGlobalElements = rebalancedOneDimensionalMap->MyGlobalElements();
  int* myGlobalElements = new int[numMyElementsUpperBound];
  int* elementSizeList = new int[numMyElementsUpperBound];
  int numPointsWithZeroNeighbors = 0;
  for(int i=0 ; i<numMyElementsUpperBound ; ++i){
    int numBonds = (int)( (*rebalancedNumberOfBonds)[i] );
    if(numBonds > 0){
      numMyElements++;
      myGlobalElements[i-numPointsWithZeroNeighbors] = rebalancedOneDimensionalMapGlobalElements[i];
      elementSizeList[i-numPointsWithZeroNeighbors] = numBonds;
    }
    else{
      numPointsWithZeroNeighbors++;
    }
  }
  int indexBase = 0;
  Teuchos::RCP<Epetra_BlockMap> rebalancedBondMap = 
    Teuchos::rcp(new Epetra_BlockMap(numGlobalElements, numMyElements, myGlobalElements, elementSizeList, indexBase, *peridigmComm));
  delete[] myGlobalElements;
  delete[] elementSizeList;

  return rebalancedBondMap;
}

Teuchos::RCP<Epetra_Vector> PeridigmNS::Peridigm::createRebalancedNeighborGlobalIDList(Teuchos::RCP<Epetra_BlockMap> rebalancedBondMap,
                                                                                       Teuchos::RCP<const Epetra_Import> bondMapToRebalancedBondMapImporter) {

  // construct a globalID neighbor list for the current decomposition
  Teuchos::RCP<Epetra_Vector> neighborGlobalIDs = Teuchos::rcp(new Epetra_Vector(*bondMap));
  int* neighborhoodList = neighborhoodData->NeighborhoodList();
  int neighborhoodListIndex = 0;
  int neighborGlobalIDIndex = 0;
  for(int i=0 ; i<neighborhoodData->NumOwnedPoints() ; ++i){
    int numNeighbors = neighborhoodList[neighborhoodListIndex++];
    for(int j=0 ; j<numNeighbors ; ++j){
      int neighborLocalID = neighborhoodList[neighborhoodListIndex++];
      (*neighborGlobalIDs)[neighborGlobalIDIndex++] = oneDimensionalOverlapMap->GID(neighborLocalID);
    }
  }

  // redistribute the globalID neighbor list to the rebalanced configuration
  Teuchos::RCP<Epetra_Vector> rebalancedNeighborGlobalIDs = Teuchos::rcp(new Epetra_Vector(*rebalancedBondMap));
  rebalancedNeighborGlobalIDs->Import(*neighborGlobalIDs, *bondMapToRebalancedBondMapImporter, Insert);

  return rebalancedNeighborGlobalIDs;
}

Teuchos::RCP<PeridigmNS::NeighborhoodData> PeridigmNS::Peridigm::createRebalancedNeighborhoodData(Teuchos::RCP<Epetra_BlockMap> rebalancedOneDimensionalMap,
                                                                                                  Teuchos::RCP<Epetra_BlockMap> rebalancedOneDimensionalOverlapMap,
                                                                                                  Teuchos::RCP<Epetra_BlockMap> rebalancedBondMap,
                                                                                                  Teuchos::RCP<Epetra_Vector> rebalancedNeighborGlobalIDs) {

  Teuchos::RCP<PeridigmNS::NeighborhoodData> rebalancedNeighborhoodData = Teuchos::rcp(new PeridigmNS::NeighborhoodData);
  rebalancedNeighborhoodData->SetNumOwned(rebalancedOneDimensionalMap->NumMyElements());
  int* ownedIDs = rebalancedNeighborhoodData->OwnedIDs();
  for(int i=0 ; i<rebalancedOneDimensionalMap->NumMyElements() ; ++i){
    int globalID = rebalancedOneDimensionalMap->GID(i);
    int localID = rebalancedOneDimensionalOverlapMap->LID(globalID);
    TEST_FOR_EXCEPTION(localID == -1, Teuchos::RangeError, "Invalid index into rebalancedOneDimensionalOverlapMap");
    ownedIDs[i] = localID;
  }
  rebalancedNeighborhoodData->SetNeighborhoodListSize(rebalancedOneDimensionalMap->NumMyElements() + rebalancedBondMap->NumMyPoints());
  // numNeighbors1, n1LID, n2LID, n3LID, numNeighbors2, n1LID, n2LID, ...
  int* neighborhoodList = rebalancedNeighborhoodData->NeighborhoodList();
  // points into neighborhoodList, gives start of neighborhood information for each locally-owned element
  int* neighborhoodPtr = rebalancedNeighborhoodData->NeighborhoodPtr();
  // gives the offset at which the list of neighbors can be found in the rebalancedNeighborGlobalIDs vector for each locally-owned element
  int* firstPointInElementList = rebalancedBondMap->FirstPointInElementList();
  // loop over locally owned points
  int neighborhoodIndex = 0;
  for(int iLID=0 ; iLID<rebalancedOneDimensionalMap->NumMyElements() ; ++iLID){
    // location of this element's neighborhood data in the neighborhoodList
    neighborhoodPtr[iLID] = neighborhoodIndex;
    // first entry is the number of neighbors
    int globalID = rebalancedOneDimensionalMap->GID(iLID);
    int rebalancedBondMapLocalID = rebalancedBondMap->LID(globalID);
    if(rebalancedBondMapLocalID != -1){
      int numNeighbors = rebalancedBondMap->ElementSize(rebalancedBondMapLocalID);
      neighborhoodList[neighborhoodIndex++] = numNeighbors;
      // next entries record the local ID of each neighbor
      int offset = firstPointInElementList[rebalancedBondMapLocalID];
      for(int iN=0 ; iN<numNeighbors ; ++iN){
        int globalNeighborID = (int)( (*rebalancedNeighborGlobalIDs)[offset + iN] );
        int localNeighborID = rebalancedOneDimensionalOverlapMap->LID(globalNeighborID);
        TEST_FOR_EXCEPTION(localNeighborID == -1, Teuchos::RangeError, "Invalid index into rebalancedOneDimensionalOverlapMap");
        neighborhoodList[neighborhoodIndex++] = localNeighborID;
      }
    }
    else{
      neighborhoodList[neighborhoodIndex++] = 0;
    }
  }

  return rebalancedNeighborhoodData;
}

void PeridigmNS::Peridigm::contactSearch(Teuchos::RCP<const Epetra_BlockMap> rebalancedOneDimensionalMap, 
                                         Teuchos::RCP<const Epetra_BlockMap> rebalancedBondMap,
                                         Teuchos::RCP<const Epetra_Vector> rebalancedNeighborGlobalIDs,
                                         PdGridData& rebalancedDecomp,
                                         Teuchos::RCP< map<int, vector<int> > > contactNeighborGlobalIDs,
                                         Teuchos::RCP< set<int> > offProcessorContactIDs) {
  // execute contact search
  rebalancedDecomp = createAndAddNeighborhood(rebalancedDecomp, contactSearchRadius);

  int* searchNeighborhood = rebalancedDecomp.neighborhood.get();
  int* searchGlobalIDs = rebalancedDecomp.myGlobalIDs.get();
  int searchListIndex = 0;
  for(int iPt=0 ; iPt<rebalancedDecomp.numPoints ; ++iPt){

    int globalID = searchGlobalIDs[iPt];
    vector<int>& contactNeighborGlobalIDList = (*contactNeighborGlobalIDs)[globalID];

    // create a stl::list of global IDs that this point is bonded to
    list<int> bondedNeighbors;
    int tempLocalID = rebalancedBondMap->LID(globalID);
    // if there is no entry in rebalancedBondMap, then there are no bonded neighbors for this point
    if(tempLocalID != -1){
      int firstNeighbor = rebalancedBondMap->FirstPointInElementList()[tempLocalID];
      int numNeighbors = rebalancedBondMap->ElementSize(tempLocalID);
      for(int i=0 ; i<numNeighbors ; ++i){
        int neighborGlobalID = (int)( (*rebalancedNeighborGlobalIDs)[firstNeighbor + i] );
        bondedNeighbors.push_back(neighborGlobalID);
      }
    }

    // loop over the neighbors found by the contact search
    // retain only those neighbors that are not bonded
    int searchNumNeighbors = searchNeighborhood[searchListIndex++];
    for(int iNeighbor=0 ; iNeighbor<searchNumNeighbors ; ++iNeighbor){
      int globalNeighborID = searchNeighborhood[searchListIndex++];
      list<int>::iterator it = find(bondedNeighbors.begin(), bondedNeighbors.end(), globalNeighborID);
      if(it == bondedNeighbors.end()){
        contactNeighborGlobalIDList.push_back(globalNeighborID);
        if(rebalancedOneDimensionalMap->LID(globalNeighborID) == -1)
          offProcessorContactIDs->insert(globalNeighborID);
      }
    }
  }
}

Teuchos::RCP<PeridigmNS::NeighborhoodData> PeridigmNS::Peridigm::createRebalancedContactNeighborhoodData(Teuchos::RCP<map<int, vector<int> > > contactNeighborGlobalIDs,
                                                                                                         Teuchos::RCP<const Epetra_BlockMap> rebalancedOneDimensionalMap,
                                                                                                         Teuchos::RCP<const Epetra_BlockMap> rebalancedOneDimensionalOverlapMap) {

  Teuchos::RCP<PeridigmNS::NeighborhoodData> rebalancedContactNeighborhoodData = Teuchos::rcp(new PeridigmNS::NeighborhoodData);
  // record the owned IDs
  rebalancedContactNeighborhoodData->SetNumOwned(rebalancedOneDimensionalMap->NumMyElements());
  int* ownedIDs = rebalancedContactNeighborhoodData->OwnedIDs();
  for(int i=0 ; i<rebalancedOneDimensionalMap->NumMyElements() ; ++i){
    int globalID = rebalancedOneDimensionalMap->GID(i);
    int localID = rebalancedOneDimensionalOverlapMap->LID(globalID);
    TEST_FOR_EXCEPTION(localID == -1, Teuchos::RangeError, "Invalid index into rebalancedOneDimensionalOverlapMap");
    ownedIDs[i] = localID;
  }
  // determine the neighborhood list size
  int neighborhoodListSize = 0;
  for(map<int, vector<int> >::const_iterator it=contactNeighborGlobalIDs->begin() ; it!=contactNeighborGlobalIDs->end() ; it++)
    neighborhoodListSize += it->second.size() + 1;
  rebalancedContactNeighborhoodData->SetNeighborhoodListSize(neighborhoodListSize);
  // numNeighbors1, n1LID, n2LID, n3LID, numNeighbors2, n1LID, n2LID, ...
  int* neighborhoodList = rebalancedContactNeighborhoodData->NeighborhoodList();
  // points into neighborhoodList, gives start of neighborhood information for each locally-owned element
  int* neighborhoodPtr = rebalancedContactNeighborhoodData->NeighborhoodPtr();
  // loop over locally owned points
  int neighborhoodIndex = 0;
  for(int iLID=0 ; iLID<rebalancedOneDimensionalMap->NumMyElements() ; ++iLID){
    // location of this element's neighborhood data in the neighborhoodList
    neighborhoodPtr[iLID] = neighborhoodIndex;
    // get the global ID of this point and the global IDs of its neighbors
    int globalID = rebalancedOneDimensionalMap->GID(iLID);
    // require that this globalID be present as a key into contactNeighborGlobalIDs
    TEST_FOR_EXCEPTION(contactNeighborGlobalIDs->count(globalID) == 0, Teuchos::RangeError, "Invalid index into contactNeighborGlobalIDs");
    const vector<int>& neighborGlobalIDs = (*contactNeighborGlobalIDs)[globalID];
    // first entry in the neighborhoodlist is the number of neighbors
    neighborhoodList[neighborhoodIndex++] = (int) neighborGlobalIDs.size();    
    // next entries record the local ID of each neighbor
    for(unsigned int iNeighbor=0 ; iNeighbor<neighborGlobalIDs.size() ; ++iNeighbor){
      neighborhoodList[neighborhoodIndex++] = rebalancedOneDimensionalOverlapMap->LID( neighborGlobalIDs[iNeighbor] );
    }
  }

  return rebalancedContactNeighborhoodData;
}
