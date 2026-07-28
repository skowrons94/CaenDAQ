#pragma once
//
// BoardInfo — a portable snapshot of a digitizer's identity and readout
// geometry, independent of the CAEN headers so it can be filled by the mock
// source too and embedded in the .caendat file header.
//
// Populated from CAEN_DGTZ_BoardInfo_t + the version/DPP detection that
// class_caen_dgtz performs at Activate() time.
//
#include <cstdint>
#include <string>

namespace caendaq {

// Mirrors CAEN_DGTZ_DPPFirmware_t so the numeric value stored in the file is
// stable and meaningful regardless of the CAEN header version.
enum class DppType : std::uint32_t {
    NotDpp = 0xFFFFFFFFu, // CAEN uses -1; stored as 0xFFFFFFFF
    Std    = 0,
    PHA    = 1,
    PSD    = 2,
    CI     = 3,
    ZLE    = 4,
    QDC    = 5,
    DAW    = 6,
};

// Role of a board in a synchronised (daisy-chained) multi-board system.
// Derived from the board's own Acquisition Control register, never configured
// separately: a board in a non-SW start mode is part of the chain, and the one
// that fires the software trigger is the master.
enum class SyncRole : std::uint32_t {
    Independent = 0, // SW-controlled start mode — runs on its own command
    Master      = 1, // in the chain, and fires the software trigger that starts it
    Slave       = 2, // in the chain, started by the trigger arriving on TRG-IN
};

inline const char* toString(SyncRole r) {
    switch (r) {
        case SyncRole::Master: return "master";
        case SyncRole::Slave:  return "slave";
        default:               return "independent";
    }
}

struct BoardInfo {
    std::string   modelName;                 // e.g. "V1730"
    std::uint32_t model            = 0;       // derived code: 725, 730, 724, 720, 751 ...
    std::uint32_t familyCode       = 0;
    std::uint32_t formFactor       = 0;       // 0 = VME64, 1 = VME64X, 2 = Desktop, 3 = NIM
    std::uint32_t channels         = 0;
    std::uint32_t adcNBits         = 0;
    std::uint32_t serialNumber     = 0;
    std::uint32_t pcbRevision      = 0;
    std::uint32_t boardRegId       = 0;       // VME board id (0-15), register 0xEF08
    DppType       dppType          = DppType::NotDpp;
    std::uint8_t  nsPerSample      = 0;
    std::uint8_t  nsPerTimetag     = 0;
    std::string   rocFirmware;                // ROC_FirmwareRel
    std::string   amcFirmware;                // AMC_FirmwareRel
    std::string   license;                    // DPP firmware licence string
    std::uint32_t channelEnableMask = 0;      // as configured
    // Acquisition-relevant registers read back after configuration, so the run
    // metadata records what the board was ACTUALLY set to (not just the file).
    std::uint32_t acquisitionControl = 0;     // 0x8100
    std::uint32_t boardConfiguration = 0;     // 0x8000
    std::uint32_t frontPanelIOControl = 0;    // 0x811C
    std::uint32_t globalTriggerMask  = 0;     // 0x810C
    std::uint32_t trgOutEnableMask   = 0;     // 0x8110
    std::uint32_t runDelay           = 0;     // 0x8170, in board steps
    // Synchronisation role programmed for this board (see Daq::Options::sync).
    SyncRole      syncRole          = SyncRole::Independent;
    // Connection (echoed from BoardParams so the file records how it was read).
    std::int32_t  connType         = 0;
    std::int32_t  linkNum          = 0;
    std::int32_t  node             = 0;
    std::uint32_t vmeBase          = 0;
};

} // namespace caendaq
