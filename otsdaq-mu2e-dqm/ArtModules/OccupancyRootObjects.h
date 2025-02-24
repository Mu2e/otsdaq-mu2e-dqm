#ifndef _OccupancyRootObjects_h_
#define _OccupancyRootObjects_h_

#include "art/Framework/Services/Registry/ServiceHandle.h"
#include "art_root_io/TFileDirectory.h"
#include "art_root_io/TFileService.h"
#include "otsdaq/NetworkUtilities/TCPPublishServer.h"
#include <TH1F.h>
#include <TH2F.h>
#include <string>

namespace ots {

class OccupancyRootObjects {
public:
  OccupancyRootObjects(const std::string Title) : _title(Title){};
  OccupancyRootObjects(){};
  virtual ~OccupancyRootObjects(void){};
  enum { kNOcc = 40, kNOccVar = 10 };

  struct occupancyHist_ {
    TH1F *_hOccInfo[kNOcc][kNOccVar];
    TH2F *_h2DOccInfo[kNOcc][kNOccVar];

    occupancyHist_() {
      for (int i = 0; i < kNOcc; ++i) {
        for (int j = 0; j < kNOccVar; ++j) {
          _hOccInfo[i][j] = NULL;
          _h2DOccInfo[i][j] = NULL;
        }
      }
    }
  };

  const std::string _title;
  occupancyHist_ Hist;

  void BookHistos(art::ServiceHandle<art::TFileService> Tfs, size_t _nTrackTrig,
                  size_t _nCaloTrig) {

    for (unsigned int i = 0; i < _nTrackTrig; ++i) {
      art::TFileDirectory occInfoDir = Tfs->mkdir(Form("occInfoTrk_%i", i));
      this->Hist._hOccInfo[i][0] = occInfoDir.make<TH1F>(
          Form("hInstLum_%i", i),
          "distrbution of instantaneous lum; p/#mu-bunch", 1000, 1e6, 4e8);

      this->Hist._h2DOccInfo[i][0] = occInfoDir.make<TH2F>(
          Form("hNSDVsLum_%i", i),
          "inst lum vs nStrawDigi; p/#mu-bunch; nStrawDigi", 1000, 1e6, 4e8,
          5000, 0., 20000.);
      this->Hist._h2DOccInfo[i][1] =
          occInfoDir.make<TH2F>(Form("hNCDVsLum_%i", i),
                                "inst lum vs nCaloDigi; p/#mu-bunch; nCaloDigi",
                                1000, 1e6, 4e8, 5000, 0., 20000.);
    }

    for (unsigned int i = _nTrackTrig; i < _nTrackTrig * 2; ++i) {
      art::TFileDirectory occInfoDir = Tfs->mkdir(Form("occInfoHel_%i", i));
      this->Hist._hOccInfo[i][0] = occInfoDir.make<TH1F>(
          Form("hInstLum_%i", i),
          "distrbution of instantaneous lum; p/#mu-bunch", 1000, 1e6, 4e8);

      this->Hist._h2DOccInfo[i][0] = occInfoDir.make<TH2F>(
          Form("hNSDVsLum_%i", i),
          "inst lum vs nStrawDigi; p/#mu-bunch; nStrawDigi", 1000, 1e6, 4e8,
          5000, 0., 20000.);
      this->Hist._h2DOccInfo[i][1] =
          occInfoDir.make<TH2F>(Form("hNCDVsLum_%i", i),
                                "inst lum vs nCaloDigi; p/#mu-bunch; nCaloDigi",
                                1000, 1e6, 4e8, 5000, 0., 20000.);
    }

    for (unsigned int i = _nTrackTrig * 2; i < _nTrackTrig * 2 + _nCaloTrig;
         ++i) {
      art::TFileDirectory occInfoDir =
          Tfs->mkdir(Form("occInfoCaloTrig_%i", i));
      this->Hist._hOccInfo[i][0] = occInfoDir.make<TH1F>(
          Form("hInstLum_%i", i),
          "distrbution of instantaneous lum; p/#mu-bunch", 1000, 1e6, 4e8);

      this->Hist._h2DOccInfo[i][0] = occInfoDir.make<TH2F>(
          Form("hNSDVsLum_%i", i),
          "inst lum vs nStrawDigi; p/#mu-bunch; nStrawDigi", 1000, 1e6, 4e8,
          5000, 0., 20000.);
      this->Hist._h2DOccInfo[i][1] =
          occInfoDir.make<TH2F>(Form("hNCDVsLum_%i", i),
                                "inst lum vs nCaloDigi; p/#mu-bunch; nCaloDigi",
                                1000, 1e6, 4e8, 5000, 0., 20000.);
    }

    unsigned int index_last = _nTrackTrig + _nCaloTrig;
    art::TFileDirectory occInfoDir = Tfs->mkdir("occInfoGeneral");
    this->Hist._hOccInfo[index_last][0] = occInfoDir.make<TH1F>(
        Form("hInstLum_%i", index_last),
        "distrbution of instantaneous lum; p/#mu-bunch", 1000, 1e6, 4e8);

    this->Hist._h2DOccInfo[index_last][0] =
        occInfoDir.make<TH2F>(Form("hNSDVsLum_%i", index_last),
                              "inst lum vs nStrawDigi; p/#mu-bunch; nStrawDigi",
                              1000, 1e6, 4e8, 5000, 0., 20000.);
    this->Hist._h2DOccInfo[index_last][1] =
        occInfoDir.make<TH2F>(Form("hNCDVsLum_%i", index_last),
                              "inst lum vs nCaloDigi; p/#mu-bunch; nCaloDigi",
                              1000, 1e6, 4e8, 5000, 0., 20000.);
  }

  void BookHistos(TDirectory *occInfoDir, size_t _nTrackTrig,
                  size_t _nCaloTrig) {

    for (unsigned int i = 0; i < _nTrackTrig; ++i) {
      occInfoDir->mkdir(Form("occInfoTrk_%i", i));
      this->Hist._hOccInfo[i][0] = new TH1F(
          Form("hInstLum_%i", i),
          "distrbution of instantaneous lum; p/#mu-bunch", 1000, 1e6, 4e8);

      this->Hist._h2DOccInfo[i][0] =
          new TH2F(Form("hNSDVsLum_%i", i),
                   "inst lum vs nStrawDigi; p/#mu-bunch; nStrawDigi", 1000, 1e6,
                   4e8, 5000, 0., 20000.);
      this->Hist._h2DOccInfo[i][1] =
          new TH2F(Form("hNCDVsLum_%i", i),
                   "inst lum vs nCaloDigi; p/#mu-bunch; nCaloDigi", 1000, 1e6,
                   4e8, 5000, 0., 20000.);
    }

    for (unsigned int i = _nTrackTrig; i < _nTrackTrig * 2; ++i) {
      occInfoDir->mkdir(Form("occInfoHel_%i", i));
      this->Hist._hOccInfo[i][0] = new TH1F(
          Form("hInstLum_%i", i),
          "distrbution of instantaneous lum; p/#mu-bunch", 1000, 1e6, 4e8);

      this->Hist._h2DOccInfo[i][0] =
          new TH2F(Form("hNSDVsLum_%i", i),
                   "inst lum vs nStrawDigi; p/#mu-bunch; nStrawDigi", 1000, 1e6,
                   4e8, 5000, 0., 20000.);
      this->Hist._h2DOccInfo[i][1] =
          new TH2F(Form("hNCDVsLum_%i", i),
                   "inst lum vs nCaloDigi; p/#mu-bunch; nCaloDigi", 1000, 1e6,
                   4e8, 5000, 0., 20000.);
    }

    for (unsigned int i = _nTrackTrig * 2; i < _nTrackTrig * 2 + _nCaloTrig;
         ++i) {
      occInfoDir->mkdir(Form("occInfoCaloTrig_%i", i));
      this->Hist._hOccInfo[i][0] = new TH1F(
          Form("hInstLum_%i", i),
          "distrbution of instantaneous lum; p/#mu-bunch", 1000, 1e6, 4e8);

      this->Hist._h2DOccInfo[i][0] =
          new TH2F(Form("hNSDVsLum_%i", i),
                   "inst lum vs nStrawDigi; p/#mu-bunch; nStrawDigi", 1000, 1e6,
                   4e8, 5000, 0., 20000.);
      this->Hist._h2DOccInfo[i][1] =
          new TH2F(Form("hNCDVsLum_%i", i),
                   "inst lum vs nCaloDigi; p/#mu-bunch; nCaloDigi", 1000, 1e6,
                   4e8, 5000, 0., 20000.);
    }

    unsigned int index_last = _nTrackTrig + _nCaloTrig;
    occInfoDir->mkdir("occInfoGeneral");
    this->Hist._hOccInfo[index_last][0] = new TH1F(
        Form("hInstLum_%i", index_last),
        "distrbution of instantaneous lum; p/#mu-bunch", 1000, 1e6, 4e8);

    this->Hist._h2DOccInfo[index_last][0] =
        new TH2F(Form("hNSDVsLum_%i", index_last),
                 "inst lum vs nStrawDigi; p/#mu-bunch; nStrawDigi", 1000, 1e6,
                 4e8, 5000, 0., 20000.);
    this->Hist._h2DOccInfo[index_last][1] =
        new TH2F(Form("hNCDVsLum_%i", index_last),
                 "inst lum vs nCaloDigi; p/#mu-bunch; nCaloDigi", 1000, 1e6,
                 4e8, 5000, 0., 20000.);
  }
};

} // namespace ots

#endif
