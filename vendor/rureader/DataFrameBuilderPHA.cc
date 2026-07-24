#include <string>

#include "DataFrameBuilderPHA.h"

// Board families sharing the same DPP-PHA data format.
static bool IsX730Family( const std::string& name )
{
  return name.find( "730" ) != std::string::npos ||
         name.find( "725" ) != std::string::npos;
}

static bool IsX724Family( const std::string& name )
{
  return name.find( "724" ) != std::string::npos ||
         name.find( "781" ) != std::string::npos ||
         name.find( "782" ) != std::string::npos;
}

DataFrameBuilderPHA::DataFrameBuilderPHA( std::string dgtzName )
{
  fDgtzName = dgtzName;

  ProduceNumSamples( );
  ProduceFlags( );
  ProduceConfigs( );
  ProduceFormats( );
}

DataFrameBuilderPHA::~DataFrameBuilderPHA( )
{
}

void DataFrameBuilderPHA::ProduceNumSamples( )
{
  fNumSamples = 0;

  if( IsX730Family( fDgtzName ) )      fNumSamples = 8;
  else if( IsX724Family( fDgtzName ) ) fNumSamples = 2;
}

void DataFrameBuilderPHA::ProduceFlags( )
{
  if( IsX730Family( fDgtzName ) ){

    fFlags["DT"]     = 31;
    fFlags["Energy"] = 30;
    fFlags["TS"]     = 29;
    fFlags["Extras"] = 28;
    fFlags["Trace"]  = 27;

  }
  else if( IsX724Family( fDgtzName ) ){

    fFlags["DT"]     = 31;
    fFlags["Trace"]  = 30;
    fFlags["Energy"] = 29;
    fFlags["TS"]     = 28;

  }
}

void DataFrameBuilderPHA::ProduceConfigs( )
{
  if( IsX730Family( fDgtzName ) ){

    fConfigs["Extras"]     = std::make_pair( 24, 26 );
    fConfigs["AnaProbe1"]  = std::make_pair( 22, 23 );
    fConfigs["AnaProbe2"]  = std::make_pair( 20, 21 );
    fConfigs["DigProbe"]   = std::make_pair( 16, 19 );
    fConfigs["NumSamples"] = std::make_pair(  0, 15 );

  }
  else if( IsX724Family( fDgtzName ) ){

    fConfigs["AnaProbe"]   = std::make_pair( 22, 23 );
    fConfigs["DigProbe1"]  = std::make_pair( 20, 21 );
    fConfigs["DigProbe2"]  = std::make_pair( 16, 19 );
    fConfigs["NumSamples"] = std::make_pair(  0, 15 );

  }
}

void DataFrameBuilderPHA::ProduceFormats( )
{
  if( IsX730Family( fDgtzName ) ){

    fFormats["Size"]   = std::make_pair(  0, 30 );
    fFormats["Energy"] = std::make_pair(  0, 14 );
    fFormats["Extras"] = std::make_pair( 16, 20 );
    fFormats["TS"]     = std::make_pair(  0, 30 );
    fFormats["CH"]     = std::make_pair( 31, 31 );
    fFormats["Sample"] = std::make_pair(  0, 13 );
    fFormats["DP1"]    = std::make_pair( 14, 14 );  // digital probe
    fFormats["DP2"]    = std::make_pair( 15, 15 );  // trigger

  }
  else if( IsX724Family( fDgtzName ) ){

    fFormats["Size"]   = std::make_pair(  0, 20 );
    fFormats["Energy"] = std::make_pair(  0, 14 );
    fFormats["Extras"] = std::make_pair( 16, 23 );
    fFormats["TS"]     = std::make_pair(  0, 29 );
    fFormats["RO"]     = std::make_pair( 31, 31 );
    fFormats["Sample"] = std::make_pair(  0, 13 );
    fFormats["DP1"]    = std::make_pair( 14, 14 );  // digital probe
    fFormats["DP2"]    = std::make_pair( 15, 15 );  // trigger

  }
}
