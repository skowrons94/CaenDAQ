#include <string>

#include "DataFrameBuilderPSD.h"

// Board families sharing the same DPP-PSD data format.
static bool IsX730Family( const std::string& name )
{
  return name.find( "730" ) != std::string::npos ||
         name.find( "725" ) != std::string::npos;
}

static bool IsX720Family( const std::string& name )
{
  return name.find( "720" ) != std::string::npos;
}

DataFrameBuilderPSD::DataFrameBuilderPSD( std::string dgtzName )
{
  fDgtzName = dgtzName;

  ProduceNumSamples( );
  ProduceFlags( );
  ProduceConfigs( );
  ProduceFormats( );
}

DataFrameBuilderPSD::~DataFrameBuilderPSD( )
{
}

void DataFrameBuilderPSD::ProduceNumSamples( )
{
  fNumSamples = 0;

  if( IsX730Family( fDgtzName ) || IsX720Family( fDgtzName ) ) fNumSamples = 8;
}

void DataFrameBuilderPSD::ProduceFlags( )
{
  if( IsX730Family( fDgtzName ) ){

    fFlags["DT"]     = 31;
    fFlags["Charge"] = 30;
    fFlags["TS"]     = 29;
    fFlags["Extras"] = 28;
    fFlags["Trace"]  = 27;

  }
  else if( IsX720Family( fDgtzName ) ){

    fFlags["DT"]     = 31;
    fFlags["Charge"] = 30;
    fFlags["TS"]     = 29;
    fFlags["Extras"] = 28;
    fFlags["Trace"]  = 27;
    fFlags["EP"]     = 26;
    fFlags["EET"]    = 22;

  }
}

void DataFrameBuilderPSD::ProduceConfigs( )
{
  if( IsX730Family( fDgtzName ) ){

    fConfigs["Extras"]     = std::make_pair( 24, 26 );
    fConfigs["AnaProbe"]   = std::make_pair( 22, 23 );
    fConfigs["DigProbe1"]  = std::make_pair( 16, 18 );
    fConfigs["DigProbe2"]  = std::make_pair( 19, 21 );
    fConfigs["NumSamples"] = std::make_pair(  0, 15 );

  }
  else if( IsX720Family( fDgtzName ) ){

    fConfigs["TrgMd"]      = std::make_pair( 24, 25 );
    fConfigs["DigProbe3"]  = std::make_pair( 19, 21 );
    fConfigs["DigProbe4"]  = std::make_pair( 16, 18 );
    fConfigs["NumSamples"] = std::make_pair(  0, 11 );

  }
}

void DataFrameBuilderPSD::ProduceFormats( )
{
  if( IsX730Family( fDgtzName ) ){

    fFormats["Size"]    = std::make_pair(  0, 21 );
    fFormats["Charge1"] = std::make_pair(  0, 14 );
    fFormats["Charge2"] = std::make_pair( 16, 31 );
    fFormats["Extras"]  = std::make_pair(  0, 31 );
    fFormats["TS"]      = std::make_pair(  0, 30 );
    fFormats["CH"]      = std::make_pair( 31, 31 );
    fFormats["Sample"]  = std::make_pair(  0, 13 );
    fFormats["DP1"]     = std::make_pair( 14, 14 );
    fFormats["DP2"]     = std::make_pair( 15, 15 );

  }
  else if( IsX720Family( fDgtzName ) ){

    fFormats["Size"]    = std::make_pair(  0, 23 );
    fFormats["Charge1"] = std::make_pair(  0, 14 );
    fFormats["Charge2"] = std::make_pair( 16, 31 );
    fFormats["Extras"]  = std::make_pair(  0, 31 );
    fFormats["TS"]      = std::make_pair(  0, 31 );
    fFormats["Sample"]  = std::make_pair(  0, 11 );
    fFormats["DP1"]     = std::make_pair( 12, 12 );
    fFormats["DP2"]     = std::make_pair( 13, 13 );
    fFormats["DP3"]     = std::make_pair( 14, 14 );
    fFormats["DP4"]     = std::make_pair( 15, 15 );

  }
}
