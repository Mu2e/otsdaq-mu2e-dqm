// -*- mode:c++ -*-
#ifndef _CaloIERCPulseFitter_h_
#define _CaloIERCPulseFitter_h_

#include "art/Framework/Services/Registry/ServiceHandle.h"
#include "art_root_io/TFileDirectory.h"
#include "art_root_io/TFileService.h"
#include "otsdaq/NetworkUtilities/TCPPublishServer.h"
#include "otsdaq/Macros/CoutMacros.h"

#include <TFile.h>
#include <TSpline.h>
#include <TF1.h>
#include <TFitResult.h>
#include <TGraph.h>
#include <TGraphErrors.h>
#include <TVirtualFitter.h>

#include "Offline/RecoDataProducts/inc/CaloDigi.hh"

#include <string>

namespace ots {

  class CaloIERCPulseFitter {
  public:

    struct FitResult_t {
      float _time    ;
      float _time_unc;
      float _chi2    ;
      int _status    ;
      FitResult_t() : _time(-1.e10f), _time_unc(0.f), _chi2(0.f), _status(-2) {}
    };

    CaloIERCPulseFitter(const char* file_name, const int diag = 0) : _diag(diag), _f(nullptr), _spline(nullptr), _f_spline(nullptr) {
      if(std::string(file_name) == "") return;
      _f = TFile::Open(file_name, "READ");
      if(!_f) {
	std::cout << "[CaloIERCPulseFitter::" << __func__ << "] No file " << file_name << " found\n";
	return;
      }
      _spline = (TSpline3*) _f->Get("spline_0_1");
      if(!_spline) {
	std::cout << "[CaloIERCPulseFitter::" << __func__ << "] No template found in spline file\n";
	return;
      }
      _f_spline = new TF1("f_spline",
			  [this](double *x, double *par) {return par[0]*this->_spline->Eval(x[0]-par[1])+par[2];}
			  ,3.,25.,3);
      _f_spline->SetParNames("scale","tpeak","ped");
      _f_spline->SetRange(_spline->GetXmin(),_spline->GetXmax());
      _f_spline->SetNpx(10000);
      TVirtualFitter::SetDefaultFitter("Minuit");
    };

    virtual ~CaloIERCPulseFitter(void){
      _f->Close();
      delete _f_spline;
    };

    FitResult_t fit_pulse(const mu2e::CaloDigi* digi) {
      FitResult_t fit_result;
      if(!digi) {
	std::cout << "[CaloIERCPulseFitter::" << __func__ << "] Null digi!\n";
	return fit_result;
      }
      if(!_f_spline) {
	std::cout << "[CaloIERCPulseFitter::" << __func__ << "] Null spline function!\n";
	return fit_result;
      }

      // Create a graph of the waveform
      TGraphErrors *gadc = new TGraphErrors();
      auto waveform = digi->waveform();
      for (size_t gi=0; gi<waveform.size(); gi++){
	gadc->SetPoint(gi, gi, waveform[gi]);
	gadc->SetPointError(gi, 0., 1.);
      }

      // Fit the waveform
      _f_spline->SetParameters(3850.,0.,0.); //initialize the default parameters FIXME: Make these fields
      // const int fitStatus = gadc->Fit("f_spline","QRN"); // fit the waveform
      const auto result = gadc->Fit("f_spline",(_diag > 1) ? "RNS" : "QRNS"); // fit the waveform
      if(result >= 0) {
	fit_result._time = 5.f*(digi->t0()+_f_spline->GetParameter(1));
	fit_result._time_unc = 5.f*result->Error(1);
	fit_result._chi2 = result->Chi2();
	fit_result._status = result->Status();
	if(_diag > 1) std::cout << "[CaloIERCPulseFitter::" << __func__ << "] Fit result: time = " << fit_result._time << " +- " << fit_result._time_unc << " status = " << fit_result._status << std::endl;
      } else {
	if(_diag > 0) std::cout << "[CaloIERCPulseFitter::" << __func__ << "] Fit failed!\n";
	fit_result._status = -1;
      }

      // Cleanup
      delete gadc;

      // Return the results
      return fit_result;
    }

  private:
    const int _diag;
    TFile*    _f;
    TSpline3* _spline;
    TF1*      _f_spline;
  };

} // namespace ots

#endif
