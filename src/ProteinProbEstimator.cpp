/*
 * ProteinProbEstimator.cpp
 *
 *  Created on: Feb 25, 2011
 *      Author: tomasoni
 */

/*******************************************************************************
 Copyright 2006-2009 Lukas Käll <lukas.kall@cbr.su.se>

 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

 http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.

 *******************************************************************************/

#include <iostream>
#include <fstream>
#include <limits>
#include "ProteinProbEstimator.h"
#include "ProteinProbEstimatorHelper.h"
#include "Globals.h"


/**
 * populates a hash table that associates the name of a protein with the list
 * of unique peptides that were associated to it by percolator. Part of this
 * same information (less conveniently indexed) is stored and manipulated by
 * fido in the proteinGraph field
 *
 * @param fullset set of unique peptides with scores computed by Percolator
 * @param proteinsToPeptides hash table to be populated
 */
void populateProteinsToPeptidesTable(Scores* fullset,
    ProteinProbEstimator* thisEstimator){
  vector<ScoreHolder>::iterator peptIt = fullset->scores.begin();
  // for each peptide
  for(; peptIt < fullset->scores.end();peptIt++){
    set<string>::iterator protIt = peptIt->pPSM->proteinIds.begin();
    // for each protein
    for(; protIt != peptIt->pPSM->proteinIds.end(); protIt++){
      // look for it in the hash table
      map<string, vector<ScoreHolder*> >::iterator found =
          thisEstimator->proteinsToPeptides.find(*protIt);
      if(found == thisEstimator->proteinsToPeptides.end()){
        // if not found insert a new protein-peptide pair...
        vector<ScoreHolder*> peptides;
        peptides.push_back(&*peptIt);
        pair<string,vector<ScoreHolder*> > protPeptPair(*protIt, peptides);
        thisEstimator->proteinsToPeptides.insert(protPeptPair);
      } else {
        // ... otherwise update
        found->second.push_back(&*peptIt);
      }
    }

  }
}

ProteinProbEstimator::ProteinProbEstimator(double alpha_par, double beta_par) {
  peptideScores = 0;
  proteinGraph = 0;
  gamma = 0.5;
  alpha = alpha_par; // 0.1;
  beta = beta_par; // 0.01;
}

/**
 * sets alpha and beta to default values (avoiding the need for grid search)
 */
void ProteinProbEstimator::setDefaultParameters(){
  alpha = 0.1;
  beta = 0.01;
}

bool ProteinProbEstimator::initialize(Scores* fullset){
  peptideScores = fullset;
  populateProteinsToPeptidesTable(fullset, this);
  bool scheduleGridSearch;
  if(alpha ==-1 || beta ==-1) scheduleGridSearch = true;
  else scheduleGridSearch = false;
  return scheduleGridSearch;
}

/**
 * writes protein weights to file.
 *
 * @param proteinGraph proteins and associated probabilities to be outputted
 * @param fileName file that will store the outputted information
 */
void writeOutputToFile(fidoOutput output, string fileName) {
  ofstream of(fileName.c_str());
  int size = output.peps.size();
  for (int k=0; k<size; k++) {
    of << output.peps[k] << " " << output.protein_ids[k] << endl;
  }
  of.close();
}

/**
 * writes protein weights to cerr.
 *
 * @param proteinGraph proteins and associated probabilities to be outputted
 */
void ProteinProbEstimator::writeOutput(fidoOutput output) {
  int size = output.size();
  for (int k=0; k<size; k++) {
    cerr << output.peps[k] << " " << output.protein_ids[k] << endl;
  }
}

/**
 * after calculating protein level probabilities, the output is stored in a
 * dedicated structure that can be printed out or evaluated during the grid
 * search
 *
 * @param proteinGraph graph with proteins and corresponding probabilities
 * calculated by fido
 * @return output results of fido encapsulated in a fidoOutput structure
 */
fidoOutput buildOutput(GroupPowerBigraph* proteinGraph){
  // array containing the PEPs
  Array<double> sorted = proteinGraph->probabilityR;
  assert(sorted.size()!=0);

  // arrays that (will) contain protein ids and corresponding indexes
  Array< Array<string> > protein_ids =  Array< Array<string> >(sorted.size());
  Array<int> indices = sorted.sort();
  // array that (will) contain q-values
  Array<double> qvalues = Array<double>(sorted.size());
  double sumPepSoFar = 0;

  // filling the protein_ids and qvalues arrays
  for (int k=0; k<sorted.size(); k++) {
    protein_ids[k] = proteinGraph->groupProtNames[indices[k]];
    // q-value: average pep of the protains scoring better than the current one
    sumPepSoFar += sorted[k];
    qvalues[k] = sumPepSoFar/(k+1);
  }
  return fidoOutput(sorted, protein_ids, qvalues);
}

/**
 * point in the space generated by the possible values of the parameters alpha
 * and beta. Each point has two coordinates, two lists of protein names (the
 * true positives and false negatives calculated by fido with the particular
 * choice of alpha and beta) and the value of the objective function calculated
 * in that point
 */
struct gridPoint {
    gridPoint(){
      objectiveFnValue = -1;
    }
    gridPoint(double alpha_par, double beta_par){
      objectiveFnValue = -1;
      alpha = alpha_par;
      beta = beta_par;
      truePositives = Array<string>();
      falsePositives = Array<string>();
    }
    void calculateObjectiveFn(double lambda, ProteinProbEstimator* toBeTested);
    bool operator >(const gridPoint& rhs) const {
      assert(objectiveFnValue!=-1);
      assert(rhs.objectiveFnValue!=-1);
      if(objectiveFnValue > rhs.objectiveFnValue)
        return true;
      else return false;
    }
    Array<string> truePositives;
    Array<string> falsePositives;
    double objectiveFnValue;
  private:
    double alpha;
    double beta;
};

/**
 * pupulates the list of true positive and false negative proteins by looking
 * at the label (decoy or target) of the peptides associated with the protein
 * in the proteinsToPeptides hash table
 */
void populateTPandFNLists(gridPoint* point, fidoOutput output,
    ProteinProbEstimator* toBeTested){
  assert(point->truePositives.size()==0);
  assert(point->falsePositives.size()==0);
  map<string, vector<ScoreHolder*> >::iterator protIt =
      toBeTested->proteinsToPeptides.begin();
  // for each protein in the proteinsToPeptides hash table
  for(; protIt != toBeTested->proteinsToPeptides.end(); protIt ++){
    bool tp = false;
    bool fp = false;
    vector<ScoreHolder*>::iterator peptIt = protIt->second.begin();
    // for each peptide associated with a certain protein
    for(; peptIt < protIt->second.end(); peptIt++){
      // check whether the peptide is target of decoy
      if((*peptIt)->label != 1) fp = true;
      else tp = true;
    }
    // if any of the associated peptides were targets, add the protein to the
    // list of true positives. If any of the associated peptides were decoys, add
    // the protein to the list of false positives.
    // (note: it might end up in both!)
    if(tp) point->truePositives.add(protIt->first);
    if(fp) point->falsePositives.add(protIt->first);
  }
}

/**
 * for a given choice of alpha and beta, calculates (1 − λ) MSE_FDR − λ ROC50
 * and stores the result in objectiveFnValue
 */
void gridPoint::calculateObjectiveFn(double lambda,
    ProteinProbEstimator* toBeTested){
  assert(alpha!=-1);
  assert(beta!=-1);
  toBeTested->alpha = alpha;
  toBeTested->beta = beta;
  fidoOutput output = toBeTested->calculateProteinProb(false);
  populateTPandFNLists(this, output, toBeTested);
  // uncomment to output the results of the probability calculation to file
  writeOutputToFile(output, "/tmp/fido/out.txt");
  ofstream o("/tmp/fido/TPFPlists.txt");
  o << falsePositives << endl << truePositives << endl;
  o.close();

  // calculate MSE_FDR
  Array<double> estimatedFdrs = Array<double>();
  Array<double> empiricalFdrs = Array<double>();
  calculateFDRs(output, truePositives, falsePositives,
      estimatedFdrs, empiricalFdrs);
  // uncomment to output the results of the MSE_FDR to file
  ofstream o1 ("/tmp/fido/FDRlists.txt");
  o1 << estimatedFdrs << endl << empiricalFdrs << endl;
  o1.close();
  double threshold = 0.1;
  double mse_fdr = calculateMSE_FDR(threshold, estimatedFdrs, empiricalFdrs);

  // calculate ROC50
  Array<int> fps = Array<int>();
  Array<int> tps = Array<int>();
  calculateRoc(output, truePositives, falsePositives, fps, tps);
  // uncomment to output the results of the ROC50 calculation to file
  ofstream o2("/tmp/fido/ROC50lists.txt");
  o2 << fps << endl << tps << endl;
  o2.close();
  int N = 50;
  double roc50 = calculateROC50(N, fps, tps);

  objectiveFnValue = (1-lambda)*mse_fdr - lambda*roc50;
}

/**
 * choose the set of parameters that jointly maximizes the ROC50 score (the
 * average sensitivity when allowing between zero and 50 false positives) and
 * minimizes the mean squared error (MSE_FDR) from an ideally calibrated
 * probability.
 * minimize: (1 − λ) MSE_FDR − λ ROC50 with λ = 0.15
 * range [0.01,0.76] at resolution of 0.05 for α
 * range [0.00,0.80] at resolution 0.05 for β
 */
void ProteinProbEstimator::gridSearchAlphaBeta(){
  // a λ approaching 1.0 will shift the emphasis to the most accurate model,
  // and a λ approaching 0.0 will result in a more calibrated model
  double lambda = 0.15;
  // before the search starts, the best point seen so far is artificially set
  // to the ideal absolute minimum
  gridPoint bestSoFar;
  bestSoFar.objectiveFnValue = numeric_limits<double>::min();
  double lower_a=0.01, upper_a=0.76;
  double lower_b=0.0, upper_b=0.80;
  // if a parameter had previously been set (from command line) exclude it from
  // the grid search
  if(alpha != -1) lower_a = upper_a = alpha;
  if(beta != -1) lower_a = upper_a = beta;

  for(double a=lower_a; a<=upper_a; a=a+0.05){
    for(double b=lower_b; b<=upper_b; b+=0.05){
      if(VERB > 2) cerr << "Testing performances with parameters: alpha = " << a
          << ", beta = "<< b << endl;
      gridPoint current = gridPoint(a,b);
      current.calculateObjectiveFn(lambda, this);
      if(VERB > 2) cerr << "Objective function value is: "
          << current.objectiveFnValue << endl;
      if(current>bestSoFar)
        if(VERB > 2) cerr << "Best choice of parameters, so far!\n\n";
        bestSoFar = current;
    }
  }
}

/**
 * Calculate protein level probabilities. By default the parameters alpha and
 * beta will be estimated by grid search. If the function is invoked with
 * gridSearch set to false, whatever values for alpha and beta were previously
 * set will be used. If no values were set, the dafault will be enforced.
 *
 * @param fullset set of unique peptides with scores computed by Percolator
 * @param gridSearch indicate whether the values of alpha and beta parameters
 * should be estimated by grid search
 */
fidoOutput ProteinProbEstimator::calculateProteinProb(bool gridSearch){
  srand(time(NULL)); cout.precision(8); cerr.precision(8);
  // by default, a grid search is executed to estimate the values of the
  // parameters alpha and beta

  if(gridSearch) {
    if(VERB > 2) cerr << "Estimating parameters for the model by grid search\n";
    gridSearchAlphaBeta();
    if(VERB > 2) {
      cerr << "The following parameters have been chosen;\n";
      cerr << "alpha = " << alpha << endl;
      cerr << "beta = " << beta << endl;
      cerr << "Protein level probabilities will now be calculated\n";
    }
  }
  // at this point the parameters alpha and beta must have been initialized:
  // either statically set through command line or set after the grid search
  // or temporarily set in one of the grid search's iteration steps
  assert(alpha != -1);
  assert(beta != -1);

  //GroupPowerBigraph::LOG_MAX_ALLOWED_CONFIGURATIONS = ;
  delete proteinGraph;
  proteinGraph = new GroupPowerBigraph ( RealRange(alpha, 1, alpha),
      RealRange(beta, 1, beta), gamma );
  proteinGraph->read(peptideScores);
  proteinGraph->getProteinProbs();

  fidoOutput output = buildOutput(proteinGraph);
  // uncomment the following line to print protein level probabilities to file
  //writeOutputToFile(output, "/tmp/fido/fidoOut.txt");
  return output;
}

/**
 * Helper function for ProteinProbEstimator::writeXML; looks for PSMs associated
 * with a protein. The protein graph is divided into subgraphs: go through each
 * of them sequentially.
 *
 * @param protein_id the protein to be located
 * @param os stream the associated peptides will be written to
 * @param proteinsToPeptides hash table containing the associations between
 * proteins and peptides as calculated by Percolator
 */
void writeXML_writeAssociatedPeptides(string& protein_id,
    ofstream& os, map<string, vector<ScoreHolder*> >& proteinsToPeptides){
  vector<ScoreHolder*>* peptides = &proteinsToPeptides.find(protein_id)->second;
  vector<ScoreHolder*>::iterator peptIt = peptides->begin();
  for(; peptIt<peptides->end(); peptIt++){
    string pept = (*peptIt)->pPSM->getPeptideNoResidues();
    os << "      <peptide_seq seq=\"" << pept << "\"/>"<<endl;
    // check that protein_id is there
    assert((*peptIt)->pPSM->proteinIds.find(protein_id)
        != (*peptIt)->pPSM->proteinIds.end());
  }
  /*
  // the following code was trying to achieve the same as the above exclusively
  // with information coming from fido (without making use of external
  // information coming from Percolator
  bool found = false;
  int peptidesIndex = -1;
  int proteinIndex = 0;
  // look for protein_id in the subgraphs
  while(peptidesIndex == -1 && proteinIndex<proteinGraph->subgraphs.size()){
    StringTable proteins;
    proteins = StringTable::AddElements(
        proteinGraph->subgraphs[proteinIndex].proteinsToPSMs.names);
    peptidesIndex = proteins.lookup(protein_id);
    if(peptidesIndex == -1) proteinIndex++;
  }
  // if it was found
  if(peptidesIndex!=-1){
    Set peptides = proteinGraph->subgraphs[proteinIndex].
        proteinsToPSMs.associations[peptidesIndex];
    // for each PSM associated with the protein, print the peptides
    for (int k=0; k<peptides.size(); k++) {
      string pept = proteinGraph->subgraphs[proteinIndex].
          PSMsToProteins.names[peptides[k]];
      os << "      <peptide_seq seq=\"" << pept << "\"/>"<<endl;
    }
  } else {
    // it seems to me that every protein should have associated peptides
    // as they were given in input to calculateProteinProb. At present this
    // is not so. Consider throwing an error here.
  }
  return;
  */
}

/**
 * output protein level probabilites results in xml format
 *
 * @param os stream to which the xml is directed
 * @param output object containing the output to be written to file
 */
void ProteinProbEstimator::writeOutputToXML(string xmlOutputFN,
    const fidoOutput& output){
  ofstream os;
  os.open(xmlOutputFN.data(), ios::app);
  // append PROTEINs
  os << "  <proteins>" << endl;
  // for each probability
  for (int k=0; k<output.size(); k++) {
    Array<string> protein_ids = output.protein_ids[k];
    // for each protein with a certain probability
    for(int k2=0; k2<protein_ids.size(); k2++) {
      string protein_id = protein_ids[k2];
      os << "    <protein p:protein_id=\"" << protein_id << "\">" << endl;
      os << "      <pep>" << output.peps[k] << "</pep>" << endl;
      os << "      <q_value>" << output.qvalues[k] << "</q_value>" << endl;
      writeXML_writeAssociatedPeptides(protein_id, os, proteinsToPeptides);
      os << "    </protein>" << endl;
    }
  }
  os << "  </proteins>" << endl << endl;
  os.close();
}
