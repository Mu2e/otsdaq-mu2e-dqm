# ======================================================================
#  otsdaq-mu2e-dqm main build file
#
#  cd .../path/to/build/directory
#  source .../path/to/otsdaq-mu2e-dqm/ups/setup_for_development
#  buildtool
# ======================================================================

cmake_minimum_required (VERSION 3.19 FATAL_ERROR)

find_package(cetmodules 3.16.00 REQUIRED)

project(otsdaq_mu2e_dqm VERSION 3.04.00)

include(CetCMakeEnv)
cet_cmake_env()
SET(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

cet_set_compiler_flags(DIAGS VIGILANT
 WERROR
 NO_UNDEFINED
 EXTRA_FLAGS -pedantic -Wno-unused-parameter -Wno-ignored-qualifiers
 )

#string(TOUPPER ${CMAKE_BUILD_TYPE} BTYPE_UC )
#if( ${BTYPE_UC} MATCHES "DEBUG" )
#  cet_add_compiler_flags(-fsanitize=address)
#endif()

#cet_report_compiler_flags()

# these are minimum required versions, not the actual product versions
find_package(otsdaq_mu2e 1.02.00 REQUIRED)

#find_package(art_root_io 1.10.01 REQUIRED)

# Mu2e Offline Extra setup
find_package(Offline v11_00_00 REQUIRED)
include_directories($ENV{BTRK_INC} $ENV{KINKAL_INC})
link_directories($ENV{BTRK_LIB} $ENV{KINKAL_LIB})

 # XDAQ Extra setup
 include_directories($ENV{XDAQ_ROOT}/include/linux $ENV{XDAQ_ROOT}/include)
link_directories($ENV{XDAQ_ROOT}/lib)


include(BuildPlugins)

# Code
add_subdirectory(otsdaq-mu2e-dqm)

# Useful scripts
add_subdirectory(tools)

# Test Programs
add_subdirectory(test)

# Documentation directory
#add_subdirectory(doc)

# Data
#FILE(COPY ConfigurationDataExamples DESTINATION Data)
#INSTALL(DIRECTORY ConfigurationDataExamples DESTINATION ${product}/${version}/Data)
#FILE(COPY XDAQConfigurations DESTINATION Data)
#INSTALL(DIRECTORY XDAQConfigurations DESTINATION ${product}/${version}/Data)
#FILE(COPY ServiceData DESTINATION Data)
#INSTALL(DIRECTORY ServiceData DESTINATION ${product}/${version}/Data)
#FILE(COPY Data DESTINATION .)
#INSTALL(DIRECTORY Data DESTINATION ${product}/${version})
#FILE(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Data/ServiceData/LoginData)
#FILE(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Data/ServiceData/LoginData/UsersData)
#FILE(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Data/ServiceData/LoginData/UsersData/UserPreferencesData)
#FILE(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Data/ServiceData/LoginData/UsersData/UserLoginHistoryData)
#FILE(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Data/ServiceData/LoginData/HashesData)
#FILE(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Data/ServiceData/LoginData/bkup/UsersData)
#FILE(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Data/ServiceData/LoginData/bkup/UsersData/UserPreferencesData)
#FILE(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Data/ServiceData/LoginData/bkup/UsersData/UserLoginHistoryData)
#FILE(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Data/ServiceData/LoginData/bkup/HashesData)
#FILE(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Data/ServiceData/LogbookData)
#FILE(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Data/ServiceData/LogbookData/logbooks)
#FILE(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Data/ServiceData/LogbookData/previews)
#INSTALL(DIRECTORY DESTINATION ${product}/${version}/Data/ServiceData/LoginData)
#INSTALL(DIRECTORY DESTINATION ${product}/${version}/Data/ServiceData/LoginData/UsersData)
#INSTALL(DIRECTORY DESTINATION ${product}/${version}/Data/ServiceData/LoginData/UsersData/UserPreferencesData)
#INSTALL(DIRECTORY DESTINATION ${product}/${version}/Data/ServiceData/LoginData/UsersData/UserLoginHistoryData)
#INSTALL(DIRECTORY DESTINATION ${product}/${version}/Data/ServiceData/LoginData/HashesData)
#INSTALL(DIRECTORY DESTINATION ${product}/${version}/Data/ServiceData/LoginData/bkup/UsersData)
#INSTALL(DIRECTORY DESTINATION ${product}/${version}/Data/ServiceData/LoginData/bkup/UsersData/UserPreferencesData)
#INSTALL(DIRECTORY DESTINATION ${product}/${version}/Data/ServiceData/LoginData/bkup/UsersData/UserLoginHistoryData)
#INSTALL(DIRECTORY DESTINATION ${product}/${version}/Data/ServiceData/LoginData/bkup/HashesData)
#INSTALL(DIRECTORY DESTINATION ${product}/${version}/Data/ServiceData/LogbookData)
#INSTALL(DIRECTORY DESTINATION ${product}/${version}/Data/ServiceData/LogbookData/logbooks)
#INSTALL(DIRECTORY DESTINATION ${product}/${version}/Data/ServiceData/LogbookData/previews)

cet_cmake_config()
