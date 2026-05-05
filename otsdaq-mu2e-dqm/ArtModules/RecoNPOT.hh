#ifndef __MU2EDQM__RECONPOT__
#define __MU2EDQM__RECONPOT__

///////////////////////////////////////////////////////////////////////////////
// Estimate N(POT) from observables
// H. Applegate, 2025
///////////////////////////////////////////////////////////////////////////////

//Reco DataProducts
#include "Offline/RecoDataProducts/inc/RecoProtonBunchIntensity.hh"
#include "Offline/RecoDataProducts/inc/IntensityInfoTimeCluster.hh"
#include "Offline/RecoDataProducts/inc/IntensityInfoTrackerHits.hh"
#include "Offline/RecoDataProducts/inc/IntensityInfoCalo.hh"

#include <cmath>
#include <memory>
#include <iostream>

namespace mu2e {

  class RecoNPOT {

  public:

    //Functions to reconstruct N(POT) from an observable
    static unsigned long long POTfromCaloEnergy  (const double caloEnergy);
    static unsigned long long POTfromCaloHits    (const int caloHits     );
    static unsigned long long POTfromTrackerHits (const int trackerHits  );
    static unsigned long long POTfromTZTCs       (const int nTCs         );
    static unsigned long long POTfromDFTCs       (const int nTCs         );
    static unsigned long long POTfromCAPHRI       (const int nCPHRhits   );
    static unsigned long long POTfromnHitsD0      (const int ncaloD0     );
    static unsigned long long POTfromnHitsD1      (const int ncaloD1     );

  };


  //-----------------------------------------------------------------------------
  // predict nPOT from CaloEnergy Observable
  unsigned long long RecoNPOT::POTfromCaloEnergy(const double caloEnergy){
    unsigned long long POT(0);

    if (caloEnergy >= 0.) {
      POT = 679158. + 12713.9*std::pow(caloEnergy,1) + 0.250061*std::pow(caloEnergy,2) + 3.70049e-06 *std::pow(caloEnergy,3) ;
    }
    return POT;
  }

  unsigned long long RecoNPOT::POTfromTrackerHits(const int trackerHits){
    unsigned long long POT(0);
    if (trackerHits >= 0.) {
      POT = 2.88904e+06 + 12964.3*std::pow(trackerHits,1) +  0.629889*std::pow(trackerHits,2) +  -2.3262e-05 *std::pow(trackerHits,3) ;
    }
    return POT;
  }
  unsigned long long RecoNPOT::POTfromTZTCs(const int nTCs){
    unsigned long long POT(0);
    if (nTCs >= 0. && nTCs <= 19.75) {

      POT = 8.17978e+06 + 2.37689e+06*std::pow(nTCs,1) + 62149*std::pow(nTCs,2) + 2141.02*std::pow(nTCs,3) ;
    }
    else {
      POT = 1.28729e+09 - 1.69468e+08*std::pow(nTCs,1) + 7.69092e+06*std::pow(nTCs,2) - 109222*std::pow(nTCs,3) ;
    }
    return POT;
  }

  unsigned long long RecoNPOT::POTfromDFTCs(const int nTCs){
    unsigned long long POT(0);
    if (nTCs >= 0.){
      POT = 1.31667e+07 + 3.85288e+06*std::pow(nTCs,1) + 278769*std::pow(nTCs,2) - 7707.41*std::pow(nTCs,3) ;
    }
    return POT;
  }

  unsigned long long RecoNPOT::POTfromCaloHits(const int nHits){
    unsigned long long POT(0);
    if (nHits >=0.0 && nHits<=2187.5 ){
      POT = 496841 + 25110.2*std::pow(nHits,1) + 1.46611*std::pow(nHits,2) + 0.000210587*std::pow(nHits,3) ;

    } else {
      POT = -3.96376e+07 + 71761.3*std::pow(nHits,1) - 16.9264*std::pow(nHits,2) +  0.00270155*std::pow(nHits,3) ;
    }
    return POT;
  }

  unsigned long long RecoNPOT::POTfromCAPHRI(const int nCPHRhits){
    unsigned long long POT(0);

    if (nCPHRhits >= 0.) {
      POT = 1.51551e+07 + 3.93948e+06*std::pow(nCPHRhits,1) + 552745*std::pow(nCPHRhits,2) + -16288.1*std::pow(nCPHRhits,3) ;
    }
    return POT;
  }


  unsigned long long RecoNPOT::POTfromnHitsD0(const int ncaloD0){
    unsigned long long POT(0);
    if (ncaloD0 >= 0. ){
      POT =  -138326 + 40566.5*std::pow(ncaloD0,1) + -3.81144*std::pow(ncaloD0,2) + 0.00415502*std::pow(ncaloD0,3) ;
    }
    return POT;
  }

  unsigned long long RecoNPOT::POTfromnHitsD1(const int ncaloD1){
    unsigned long long POT(0);

    if (ncaloD1 >= 0.) {
      POT =1.02265e+06 + 81212.6*std::pow(ncaloD1,1) +7.81749*std::pow(ncaloD1,2) + 0.00396047*std::pow(ncaloD1,3) ;
    }
    return POT;
  }
}

#endif
