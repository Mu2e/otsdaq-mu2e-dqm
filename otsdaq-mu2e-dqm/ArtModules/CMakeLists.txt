
# cet_build_plugin(Occupancy art::module LIBRARIES REG
#   )

# cet_build_plugin(TrackerDQM art::module LIBRARIES REG
#   )

cet_build_plugin(CaloDQM art::module LIBRARIES REG
art_root_io::TFileService_service
artdaq-core-mu2e::Data
otsdaq-mu2e::ArtModules
otsdaq::NetworkUtilities
Offline::RecoDataProducts
ROOT::Hist
ROOT::Tree
ROOT::Core
ROOT::RIO
ROOT::Gui
)

cet_build_plugin(IntensityInfoDQM art::module LIBRARIES REG
art_root_io::TFileService_service
artdaq-core-mu2e::Data
otsdaq-mu2e::ArtModules
otsdaq::NetworkUtilities
Offline::RecoDataProducts
ROOT::Hist
ROOT::Tree
ROOT::Core
ROOT::RIO
ROOT::Gui
)

cet_build_plugin(TrackerDQM art::module LIBRARIES REG
art_root_io::TFileService_service
artdaq-core-mu2e::Data
otsdaq-mu2e::ArtModules
otsdaq::NetworkUtilities
Offline::DataProducts
Offline::RecoDataProducts
Offline::TrkHitReco
ROOT::Hist
ROOT::Tree
ROOT::Core
ROOT::RIO
ROOT::Gui
)

# BTrk and KinKal have non-standard Find*.cmake...
include_directories($ENV{KINKAL_INC})
include_directories($ENV{BTRK_INC})
link_directories($ENV{KINKAL_LIB})
link_directories($ENV{BTRK_LIB})

cet_build_plugin(TriggerDQM art::module LIBRARIES REG
art_root_io::TFileService_service
artdaq-core-mu2e::Data
canvas::canvas
otsdaq-mu2e::ArtModules
otsdaq::NetworkUtilities
Offline::Mu2eUtilities
ROOT::Hist
ROOT::Tree
ROOT::Core
ROOT::RIO
ROOT::Gui
)


cet_build_plugin(SimpleDQM art::module LIBRARIES REG
art_root_io::TFileService_service
artdaq-core-mu2e::Data
canvas::canvas
otsdaq-mu2e::ArtModules
otsdaq::NetworkUtilities
Offline::Mu2eUtilities
ROOT::Hist
ROOT::Tree
ROOT::Core
ROOT::RIO
ROOT::Gui
)

cet_build_plugin(ReadTriggerCounts art::module LIBRARIES REG
art_root_io::TFileService_service
artdaq-core-mu2e::Data
	artdaq::ArtModules
Offline::BFieldGeom
      Offline::ConditionsService
      Offline::DataProducts
      Offline::GeometryService
      Offline::GlobalConstantsService
      Offline::Mu2eUtilities
      Offline::RecoDataProducts
      Offline::TrackerGeom
	)

cet_build_plugin(CaloIERCDQM art::module LIBRARIES REG
art_root_io::TFileService_service
artdaq_core_mu2e::artdaq-core-mu2e_Data
otsdaq_mu2e::otsdaq-mu2e_ArtModules
otsdaq::NetworkUtilities
Offline::DataProducts
Offline::RecoDataProducts
Offline::TrkHitReco
ROOT::Hist
ROOT::Tree
ROOT::Core
ROOT::RIO
ROOT::Gui
)

cet_build_plugin(CaloSiDETDQM art::module LIBRARIES REG
art_root_io::TFileService_service
artdaq_core_mu2e::artdaq-core-mu2e_Data
otsdaq_mu2e::otsdaq-mu2e_ArtModules
otsdaq::NetworkUtilities
Offline::DataProducts
Offline::RecoDataProducts
Offline::TrkHitReco
Offline::DAQ
Offline::CaloConditions
ROOT::Hist
ROOT::Tree
ROOT::Core
ROOT::RIO
ROOT::Gui
)

cet_build_plugin(IntegrationDQM art::module LIBRARIES REG
art_root_io::TFileService_service
artdaq_core_mu2e::artdaq-core-mu2e_Data
otsdaq_mu2e::otsdaq-mu2e_ArtModules
otsdaq::NetworkUtilities
Offline::DataProducts
Offline::RecoDataProducts
Offline::TrkHitReco
ROOT::Hist
ROOT::Tree
ROOT::Core
ROOT::RIO
ROOT::Gui
)

install_headers()
install_source()
install_fhicl(SUBDIRS fcl)
