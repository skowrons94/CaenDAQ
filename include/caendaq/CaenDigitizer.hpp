#pragma once
//
// CaenDigitizer — the real IDigitizer backend, a thin adapter over the proven
// class_caen_dgtz driver (vendored under vendor/caen/). All the board-version
// detection, calibration, DPP mandatory-bit handling and register configuration
// from the original XDAQ code is preserved; this adapter just drives the
// lifecycle, exposes BoardInfo for the file header, and adds auto-reconnect.
//
// Only compiled when the project is configured with -DCAENDAQ_WITH_CAEN=ON.
//
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "caendaq/IDigitizer.hpp"

class CAENDgtz; // fwd-decl of the vendored driver (defined in vendor/caen)

namespace caendaq {

class CaenDigitizer : public IDigitizer {
public:
    explicit CaenDigitizer(BoardParams params);
    ~CaenDigitizer() override;

    bool open() override;
    bool configure() override;
    std::uint32_t startMode() const override;
    bool start() override;
    bool arm() override;
    bool sendSWTrigger() override;
    bool read(const char** data, std::size_t* size) override;
    bool writeRegister(std::uint32_t address, std::uint32_t value) override;
    bool readRegister(std::uint32_t address, std::uint32_t* value) override;
    bool stop() override;
    void close() override;

    BoardInfo info() const override { return info_; }
    const std::string& name() const override { return params_.name; }
    bool connected() const override;

private:
    bool activate();        // OpenDigitizer + capture BoardInfo
    bool applyConfig();     // register/dgtzs config + aggregation + Malloc buffer
    // Apply a WebDAQ-style flat register dump. Returns false if the file has no
    // "registers" section (so the caller falls back to the dgtzs format).
    bool applyRegisterDump(const std::string& path);
    bool reconnect();       // full recovery: activate + configure + start
    void refreshBoardInfo();

    BoardParams               params_;
    std::unique_ptr<CAENDgtz> dgtz_;
    BoardInfo                 info_;

    // One CAEN handle, two callers: the reader thread and (during online
    // tuning) whoever writes a register. The CAEN library gives no thread-safety
    // guarantee for concurrent calls on the same handle, so read() and the
    // register accessors serialise here.
    //
    // stop()/close() deliberately stay outside it: they are the recovery path
    // for a board that has stopped answering, and must not be made to wait on a
    // reader stuck inside a long ReadData.
    mutable std::mutex        ioMutex_;

    char*         readoutBuffer_ = nullptr; // owned by CAEN lib (MallocReadoutBuffer)
    std::uint32_t bufferSize_    = 0;

    int           errcode_ = 0;             // populated via SetExternalErrorCode
    std::string   errdesc_;
    bool          configured_ = false;
    bool          running_    = false;
};

} // namespace caendaq
