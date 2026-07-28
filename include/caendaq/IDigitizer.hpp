#pragma once
//
// IDigitizer — hardware abstraction for one digitizer board.
//
// The rest of the DAQ (reader thread, file writer, and later the parallel
// decoder) talks only to this interface, so the exact same pipeline runs
// against real CAEN hardware (CaenDigitizer) or a synthetic source
// (MockDigitizer) for hardware-free testing.
//
// Lifecycle mirrors the proven CAEN sequence:
//     open()  ->  configure()  ->  start()  ->  read() * N  ->  stop()  ->  close()
//
// Contract: NO method throws. Failures are reported via the bool return value
// and logged. This is central to the "never crash" requirement.
//
#include <cstddef>
#include <cstdint>
#include <string>

#include "caendaq/BoardInfo.hpp"

namespace caendaq {

struct BoardParams {
    std::string   name       = "board"; // logical name, embedded in output filenames
    int           connType   = 0;        // CAEN connection type (0=USB, 1=Optical, 5=A4818, ...)
    int           linkNum    = 0;
    int           node       = 0;
    std::uint32_t vmeBase    = 0;        // VME base address (0 for USB/desktop)
    std::string   configPath;            // JSON register-config file (empty = skip config)
};

// Acquisition Control (0x8100) bits [1:0] — Start/Stop Mode Selection.
// The board's own configuration decides these; CaenDAQ only reads them back to
// work out how each board has to be started.
enum StartMode : std::uint32_t {
    kStartModeSW         = 0, // software start/stop via bit[2]
    kStartModeSInGpi     = 1, // armed; runs while S-IN/GPI is asserted
    kStartModeFirstTrig  = 2, // armed; starts on the first TRG-IN rising edge
    kStartModeLVDS       = 3, // armed; driven by the LVDS RUN signal (VME only)
};

class IDigitizer {
public:
    virtual ~IDigitizer() = default;

    // Open the connection and read board info. False on failure.
    virtual bool open() = 0;

    // Apply the register configuration and allocate the readout buffer.
    virtual bool configure() = 0;

    // Start/Stop Mode this board is configured with, read back from
    // Acquisition Control (0x8100) bits[1:0]. Valid after configure().
    // kStartModeSW means "starts on software command"; anything else means the
    // board waits for an external start and must be ARMED instead of started.
    virtual std::uint32_t startMode() const { return kStartModeSW; }

    // Convenience: is this board waiting for an external start signal?
    bool synchronised() const { return startMode() != kStartModeSW; }

    // Clear board memory and start acquisition (software start). Only
    // meaningful for a board in kStartModeSW.
    virtual bool start() = 0;

    // Arm a synchronised board: clear its memory and set Acquisition Control
    // bit[2], so it begins acquiring the moment its external start arrives.
    // Defaults to start(), which writes the very same bit — the distinction
    // exists so backends can log and validate the two cases separately.
    virtual bool arm() { return start(); }

    // Fire a software trigger (register 0x8108). On the master of a daisy chain
    // this is what starts the whole system: the pulse is propagated on TRG-OUT
    // into the next board's TRG-IN, and so on down the chain.
    virtual bool sendSWTrigger() { return true; }

    // Read one readout buffer. On success returns true and sets *data / *size to
    // a buffer owned by the digitizer, valid until the next read()/stop()/close().
    // *size may be 0 when no data is currently available (this is NOT an error).
    // Returns false on a communication error (caller should attempt recovery).
    virtual bool read(const char** data, std::size_t* size) = 0;

    // Write one register on a board that is already open, WHILE it is being
    // read out. This is what online tuning needs: change a trigger threshold or
    // a DC offset and watch the effect, instead of stopping the run, editing the
    // config and starting again.
    //
    // The caller may be any thread, so a backend that shares a device handle
    // with the reader must serialise the two. Which registers are safe to touch
    // during acquisition is decided above this layer (the server keeps the
    // allowlist); the backend only performs the write it is asked for.
    //
    // Returns false if the board is not open or the write failed.
    virtual bool writeRegister(std::uint32_t address, std::uint32_t value) {
        (void)address;
        (void)value;
        return false;
    }

    // Read one register back. Same threading contract as writeRegister; the
    // value is written into *value. False if unavailable.
    virtual bool readRegister(std::uint32_t address, std::uint32_t* value) {
        (void)address;
        (void)value;
        return false;
    }

    // Stop acquisition (leaves the board open/configured).
    virtual bool stop() = 0;

    // Close the connection and release resources. Idempotent.
    virtual void close() = 0;

    // Board identity/geometry, valid after a successful configure() (before
    // that it may be partially populated). Embedded in the .caendat header.
    virtual BoardInfo info() const = 0;

    virtual const std::string& name() const = 0;
    virtual bool connected() const = 0;
};

} // namespace caendaq
