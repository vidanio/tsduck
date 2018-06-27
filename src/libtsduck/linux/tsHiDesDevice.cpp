//----------------------------------------------------------------------------
//
// TSDuck - The MPEG Transport Stream Toolkit
// Copyright (c) 2005-2018, Thierry Lelegard
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
// THE POSSIBILITY OF SUCH DAMAGE.
//
//----------------------------------------------------------------------------
//
//  An encapsulation of a HiDes modulator device - Linux implementation.
//
//  An insane driver:
//
//  The it950x driver is probably the worst Linux driver in terms of design
//  and interface. Here is a non-exhaustive list of discrepancies that were
//  discovered and which have an impact on the application:
//
//  1. The driver interface defines its own integer type and there are
//     INCONSISTENCIES between the int types and the associated comment.
//     Typically, the size of a 'long' depends on the platform (32 vs. 64 bits).
//     And a 'long long' is often 64-bit on 32-bit platforms despite the comment
//     (32 bits). So, there is a bug somewhere:
//     - Either the definitions are correct and consistently used in the driver
//       code and application code. And the comments are incorrect.
//     - Or the comments are correct and the definitions are broken on some
//       platforms. Extensive testing is required on 32 and 64-bit platforms.
//
//  2. The write(2) system call returns an error code instead of a size.
//     For more than 40 years, write(2) is documented as returning the number
//     of written bytes or -1 on error. In Linux kernel, the write(2) returned
//     value is computed by the driver. And the it950x driver is completely
//     insane here: It returns a status code (0 on success). Doing this clearly
//     breaks the Unix file system paradigm "a file is a file" and writing to
//     a file is a consistent operation on all file systems. Additionally, in
//     case of success, we have no clue on the written size (assume all).
//
//  3. The Linux driver cannot regulate its output. The data are written to an
//     internal buffer of the driver and control is immediately returned to
//     the application. Unlike any well-behaved driver, the driver cannot
//     suspend the application when the buffer is full, waiting for space
//     in the buffer. When the buffer is full, the write operation fails with
//     an error, forcing the application to do some polling. This is exactly
//     what a drvier should NOT do! Polling is the enemy of performance and
//     accuracy.
//
//  Implementation notes:
//
//  The documented limitation for transmission size is 348 packets. The it950x
//  driver contains an internal buffer named "URB" to store packets. The size
//  of the URB is #define URB_BUFSIZE_TX 32712 (172 packets, 348/2). To avoid
//  issues, we limit our I/O's to 172 packets at a time, the URB size.
//
//  Any write(2) operation may fail because of the absence of regulation. The
//  "normal" error is an insufficient free buffer size, error code 59. In that
//  case, the application must do some polling (wait and retry). All other
//  error codes are probably "real" errors.
//
//  First, to avoid issues in case of other "normal" error or when the error
//  code values change in a future version, we treat all errors equally. This
//  means that we always retry, but not infinitely.
//
//  Then, the challenge with polling is to wait:
//  - not too long to avoid missed deadlines and holes in the transmission,
//  - not too short to avoid excessince CPU load,
//  - not too many times to avoid hanging an application on real errors.
//
//  In the original HiDes / ITE sample test code, the application infinitely
//  retries after waiting 100 micro-seconds. This is insane...
//
//  Here, we keep track of the transmission time and bitrate since the first
//  transmitted packet. Before a write, we try to predict the amount of time
//  to wait until write will be possible without retry. Then, if retry is
//  needed anyway, we loop a few times on short waits.
//
//----------------------------------------------------------------------------

#include "tsIT950x.h"
#include "tsHiDesDevice.h"
#include "tsNullReport.h"
#include "tsMemoryUtils.h"
#include "tsSysUtils.h"
#include "tsMonotonic.h"
#include "tsNames.h"
TSDUCK_SOURCE;

using namespace ite;

// Maximum size of our transfers. See comments above.
#define ITE_MAX_SEND_PACKETS  172
#define ITE_MAX_SEND_BYTES    (ITE_MAX_SEND_PACKETS * 188)


//----------------------------------------------------------------------------
// Class internals, the "guts" internal class.
//----------------------------------------------------------------------------

class ts::HiDesDevice::Guts
{
public:
    int             fd;            // File descriptor.
    bool            transmitting;  // Transmission in progress.
    BitRate         bitrate;       // Nominal bitrate from last tune operation.
    Monotonic       due_time;      // Expected time of buffer availability.
    PacketCounter   pkt_sent;      // Total packets sent.
    uint64_t        all_write;     // Statistics: total number of write(2) operations.
    uint64_t        fail_write;    // Statistics: number of failed write(2) operations.
    HiDesDeviceInfo info;          // Portable device information.

    // Constructor, destructor.
    Guts();
    ~Guts();

    // Open a device. Index is optional.
    bool open(int index, const UString& name, Report& report);

    // Redirected services for enclosing class.
    void close();
    bool startTransmission(Report& report);
    bool stopTransmission(Report& report);
    bool send(const TSPacket* data, size_t packet_count, Report& report);

    // Get all HiDes device names.
    static void GetAllDeviceNames(UStringVector& names);

    // Get HiDes error message.
    static UString HiDesErrorMessage(ssize_t driver_status, int errno_status);
};


//----------------------------------------------------------------------------
// Guts, constructor and destructor.
//----------------------------------------------------------------------------

ts::HiDesDevice::Guts::Guts() :
    fd(-1),
    transmitting(false),
    bitrate(0),
    due_time(),
    pkt_sent(0),
    all_write(0),
    fail_write(0),
    info()
{
}

ts::HiDesDevice::Guts::~Guts()
{
    close();
}


//----------------------------------------------------------------------------
// Public class, constructor and destructor.
//----------------------------------------------------------------------------

ts::HiDesDevice::HiDesDevice() :
    _is_open(false),
    _guts(new Guts)
{
}

ts::HiDesDevice::~HiDesDevice()
{
    // Free internal resources.
    if (_guts != 0) {
        delete _guts;
        _guts = 0;
    }
}


//----------------------------------------------------------------------------
// Get HiDes error message.
//----------------------------------------------------------------------------

ts::UString ts::HiDesDevice::Guts::HiDesErrorMessage(ssize_t driver_status, int errno_status)
{
    UString msg;

    // HiDes status can be a negative value. Zero means no error.
    if (driver_status != 0) {
        msg = DVBNameFromSection(u"HiDesError", std::abs(driver_status), names::HEXA_FIRST);
    }

    // In case errno was also set.
    if (errno_status != 0 && errno_status != driver_status) {
        if (!msg.empty()) {
            msg.append(u", ");
        }
        msg.append(ErrorCodeMessage(errno_status));
    }

    return msg;
}


//----------------------------------------------------------------------------
// Get all HiDes device names.
//----------------------------------------------------------------------------

void ts::HiDesDevice::Guts::GetAllDeviceNames(UStringVector& names)
{
    // First, get all /dev/usb-it95?x* devices.
    ExpandWildcard(names, u"/dev/usb-it95?x*");

    // Then, filter out receiver devices (we keep only transmitters / modulators).
    for (auto it = names.begin(); it != names.end(); ) {
        if (it->contain(u"-rx")) {
            it = names.erase(it);
        }
        else {
            ++it;
        }
    }
}


//----------------------------------------------------------------------------
// Get all HiDes devices in the system.
//----------------------------------------------------------------------------

bool ts::HiDesDevice::GetAllDevices(HiDesDeviceInfoList& devices, Report& report)
{
    // Clear returned array.
    devices.clear();

    // Get list of devices.
    UStringVector names;
    Guts::GetAllDeviceNames(names);

    // Loop on all devices.
    for (size_t index = 0; index < names.size(); ++index) {

        // Open the device on a dummy Guts object.
        // Ignore errors. We know that index and names are correct and they
        // describe a real device. Errors come from fetching other properties.
        Guts guts;
        guts.open(index, names[index], report);

        // Push the description of the device.
        devices.push_back(guts.info);
        guts.close();
    }

    return true;
}

//----------------------------------------------------------------------------
// Open a device. Internal version.
//----------------------------------------------------------------------------

bool ts::HiDesDevice::Guts::open(int index, const UString& name, Report& report)
{
    // Reinit info structure.
    info.clear();
    info.index = index;
    info.name = BaseName(name);
    info.path = name;

    // Open the device.
    fd = ::open(name.toUTF8().c_str(), O_RDWR);
    if (fd < 0) {
        const int err = LastErrorCode();
        report.error(u"error opening %s: %s", {name, ErrorCodeMessage(err)});
        return false;
    }

    // After this point, we don't return on error, but we report the final status.
    bool status = true;

    // Get chip type.
    TxGetChipTypeRequest chipTypeRequest;
    TS_ZERO(chipTypeRequest);
    errno = 0;

    if (::ioctl(fd, IOCTL_ITE_MOD_GETCHIPTYPE, &chipTypeRequest) < 0 || chipTypeRequest.error != 0) {
        const int err = errno;
        report.error(u"error getting chip type on %s: %s", {info.path, HiDesErrorMessage(chipTypeRequest.error, err)});
        status = false;
    }
    else {
        info.chip_type = uint16_t(chipTypeRequest.chipType);
    }

    // Get device type
    TxGetDeviceTypeRequest devTypeRequest;
    TS_ZERO(devTypeRequest);
    errno = 0;

    if (::ioctl(fd, IOCTL_ITE_MOD_GETDEVICETYPE, &devTypeRequest) < 0 || devTypeRequest.error != 0) {
        const int err = errno;
        report.error(u"error getting device type on %s: %s", {info.path, HiDesErrorMessage(devTypeRequest.error, err)});
        status = false;
    }
    else {
        info.device_type = int(devTypeRequest.DeviceType);
    }

    // Get driver information.
    TxModDriverInfo driverRequest;
    TS_ZERO(driverRequest);
    errno = 0;

    if (::ioctl(fd, IOCTL_ITE_MOD_GETDRIVERINFO, &driverRequest) < 0 || driverRequest.error != 0) {
        const int err = errno;
        report.error(u"error getting driver info on %s: %s", {info.path, HiDesErrorMessage(driverRequest.error, err)});
        status = false;
    }
    else {
        // Make sure all strings are nul-terminated.
        // This may result in a sacrifice of the last character.
        // But it is still better than trashing memory.

#define TS_ZCOPY(field1, field2) \
        driverRequest.field2[sizeof(driverRequest.field2) - 1] = 0; \
        info.field1.assignFromUTF8(reinterpret_cast<const char*>(&driverRequest.field2))

        TS_ZCOPY(driver_version, DriverVerion);
        TS_ZCOPY(api_version, APIVerion);
        TS_ZCOPY(link_fw_version, FWVerionLink);
        TS_ZCOPY(ofdm_fw_version, FWVerionOFDM);
        TS_ZCOPY(company, Company);
        TS_ZCOPY(hw_info, SupportHWInfo);
    }

    // In case of error, close file descriptor.
    if (!status) {
        close();
    }
    return status;
}


//----------------------------------------------------------------------------
// Open the HiDes device.
//----------------------------------------------------------------------------

bool ts::HiDesDevice::open(int index, Report& report)
{
    // Error if already open.
    if (_is_open) {
        report.error(u"%s already open", {_guts->info.path});
        return false;
    }

    // Get all devices and check index.
    UStringVector names;
    Guts::GetAllDeviceNames(names);
    if (index < 0 || size_t(index) >= names.size()) {
        report.error(u"HiDes adapter %s not found", {index});
        return false;
    }

    // Perform opening.
    _is_open = _guts->open(index, names[index], report);
    return _is_open;
}

bool ts::HiDesDevice::open(const UString& name, Report& report)
{
    // Error if already open.
    if (_is_open) {
        report.error(u"%s already open", {_guts->info.path});
        return false;
    }

    // Perform opening. No index provided.
    _is_open = _guts->open(-1, name, report);
    return _is_open;
}


//----------------------------------------------------------------------------
// Get information about the device.
//----------------------------------------------------------------------------

bool ts::HiDesDevice::getInfo(HiDesDeviceInfo& info, Report& report) const
{
    if (_is_open) {
        info = _guts->info;
        return true;
    }
    else {
        report.error(u"HiDes device not open");
        return false;
    }
}


//----------------------------------------------------------------------------
// Close the device.
//----------------------------------------------------------------------------

bool ts::HiDesDevice::close(Report& report)
{
    // Silently ignore "already closed".
    _guts->close();
    _is_open = false;
    return true;
}

void ts::HiDesDevice::Guts::close()
{
    if (fd >= 0) {
        if (transmitting) {
            stopTransmission(NULLREP);
        }
        ::close(fd);
    }
    transmitting = false;
    fd = -1;
}


//----------------------------------------------------------------------------
// Set or get the output gain in dB.
//----------------------------------------------------------------------------

bool ts::HiDesDevice::setGain(int& gain, Report& report)
{
    if (!_is_open) {
        report.error(u"HiDes device not open");
        return false;
    }

    TxSetGainRequest request;
    TS_ZERO(request);
    request.GainValue = gain;
    errno = 0;

    if (::ioctl(_guts->fd, IOCTL_ITE_MOD_ADJUSTOUTPUTGAIN, &request) < 0 || request.error != 0) {
        const int err = errno;
        report.error(u"error setting gain on %s: %s", {_guts->info.path, Guts::HiDesErrorMessage(request.error, err)});
        return false;
    }

    // Updated value.
    gain = request.GainValue;
    return true;
}

bool ts::HiDesDevice::getGain(int& gain, Report& report)
{
    gain = 0;

    if (!_is_open) {
        report.error(u"HiDes device not open");
        return false;
    }

    TxGetOutputGainRequest request;
    TS_ZERO(request);
    errno = 0;

    if (::ioctl(_guts->fd, IOCTL_ITE_MOD_GETOUTPUTGAIN, &request) < 0 || request.error != 0) {
        const int err = errno;
        report.error(u"error getting gain on %s: %s", {_guts->info.path, Guts::HiDesErrorMessage(request.error, err)});
        return false;
    }

    // Updated value.
    gain = request.gain;
    return true;
}


//----------------------------------------------------------------------------
// Get the allowed range of output gain in dB.
//----------------------------------------------------------------------------

bool ts::HiDesDevice::getGainRange(int& minGain, int& maxGain, uint64_t frequency, BandWidth bandwidth, Report& report)
{
    minGain = maxGain = 0;

    if (!_is_open) {
        report.error(u"HiDes device not open");
        return false;
    }

    // Frequency and bandwidth are in kHz
    TxGetGainRangeRequest request;
    TS_ZERO(request);
    request.frequency = uint32_t(frequency / 1000);
    request.bandwidth = BandWidthValueHz(bandwidth) / 1000;
    errno = 0;

    if (request.bandwidth == 0) {
        report.error(u"unsupported bandwidth");
        return false;
    }

    if (::ioctl(_guts->fd, IOCTL_ITE_MOD_GETGAINRANGE, &request) < 0 || request.error != 0) {
        const int err = errno;
        report.error(u"error getting gain range on %s: %s", {_guts->info.path, Guts::HiDesErrorMessage(request.error, err)});
        return false;
    }

    maxGain = request.maxGain;
    minGain = request.minGain;
    return true;
}


//----------------------------------------------------------------------------
// Tune the modulator with DVB-T modulation parameters.
//----------------------------------------------------------------------------

bool ts::HiDesDevice::tune(const TunerParametersDVBT& params, Report& report)
{
    if (!_is_open) {
        report.error(u"HiDes device not open");
        return false;
    }

    // Build frequency + bandwidth parameters.
    TxAcquireChannelRequest acqRequest;
    TS_ZERO(acqRequest);

    // Frequency is in kHz.
    acqRequest.frequency = uint32_t(params.frequency / 1000);

    // Bandwidth is in kHz
    acqRequest.bandwidth = BandWidthValueHz(params.bandwidth) / 1000;
    if (acqRequest.bandwidth == 0) {
        report.error(u"unsupported bandwidth");
        return false;
    }

    // Build modulation parameters.
    // Translate TSDuck enums into HiDes codes.
    TxSetModuleRequest modRequest;
    TS_ZERO(modRequest);

    switch (params.modulation) {
        case QPSK:
            modRequest.constellation = Byte(Mode_QPSK);
            break;
        case QAM_16:
            modRequest.constellation = Byte(Mode_16QAM);
            break;
        case QAM_64:
            modRequest.constellation = Byte(Mode_64QAM);
            break;
        default:
            report.error(u"unsupported constellation");
            return false;
    }

    switch (params.fec_hp) {
        case FEC_1_2:
            modRequest.highCodeRate = Byte(CodeRate_1_OVER_2);
            break;
        case FEC_2_3:
            modRequest.highCodeRate = Byte(CodeRate_2_OVER_3);
            break;
        case FEC_3_4:
            modRequest.highCodeRate = Byte(CodeRate_3_OVER_4);
            break;
        case FEC_5_6:
            modRequest.highCodeRate = Byte(CodeRate_5_OVER_6);
            break;
        case FEC_7_8:
            modRequest.highCodeRate = Byte(CodeRate_7_OVER_8);
            break;
        default:
            report.error(u"unsupported high priority code rate");
            return false;
    }

    switch (params.guard_interval) {
        case GUARD_1_32:
            modRequest.interval = Byte(Interval_1_OVER_32);
            break;
        case GUARD_1_16:
            modRequest.interval = Byte(Interval_1_OVER_16);
            break;
        case GUARD_1_8:
            modRequest.interval = Byte(Interval_1_OVER_8);
            break;
        case GUARD_1_4:
            modRequest.interval = Byte(Interval_1_OVER_4);
            break;
        default:
            report.error(u"unsupported guard interval");
            return false;
    }

    switch (params.transmission_mode) {
        case TM_2K:
            modRequest.transmissionMode = Byte(TransmissionMode_2K);
            break;
        case TM_4K:
            modRequest.transmissionMode = Byte(TransmissionMode_4K);
            break;
        case TM_8K:
            modRequest.transmissionMode = Byte(TransmissionMode_8K);
            break;
        default:
            report.error(u"unsupported transmission mode");
            return false;
    }

    // Build spectral inversion parameters.
    TxSetSpectralInversionRequest invRequest;
    TS_ZERO(invRequest);
    bool setInversion = true;

    switch (params.inversion) {
        case SPINV_OFF:
            invRequest.isInversion = False;
            break;
        case SPINV_ON:
            invRequest.isInversion = True;
            break;
        case SPINV_AUTO:
            setInversion = false;
            break;
        default:
            report.error(u"unsupported spectral inversion");
            return false;
    }

    // Now all parameters are validated, call the driver.
    errno = 0;
    if (::ioctl(_guts->fd, IOCTL_ITE_MOD_ACQUIRECHANNEL, &acqRequest) < 0 || acqRequest.error != 0) {
        const int err = errno;
        report.error(u"error setting frequency & bandwidth: %s", {Guts::HiDesErrorMessage(acqRequest.error, err)});
        return false;
    }

    errno = 0;
    if (::ioctl(_guts->fd, IOCTL_ITE_MOD_SETMODULE, &modRequest) < 0 || modRequest.error != 0) {
        const int err = errno;
        report.error(u"error setting modulation parameters: %s", {Guts::HiDesErrorMessage(modRequest.error, err)});
        return false;
    }

    errno = 0;
    if (setInversion && (::ioctl(_guts->fd, IOCTL_ITE_MOD_SETSPECTRALINVERSION, &invRequest) < 0 || invRequest.error != 0)) {
        const int err = errno;
        report.error(u"error setting spectral inversion: %s", {Guts::HiDesErrorMessage(invRequest.error, err)});
        return false;
    }

    // Keep nominal bitrate.
    _guts->bitrate = params.theoreticalBitrate();
    return true;
}


//----------------------------------------------------------------------------
// Start transmission (after having set tuning parameters).
//----------------------------------------------------------------------------

bool ts::HiDesDevice::startTransmission(Report& report)
{
    if (!_is_open) {
        report.error(u"HiDes device not open");
        return false;
    }
    else {
        return _guts->startTransmission(report);
    }
}

bool ts::HiDesDevice::Guts::startTransmission(Report& report)
{
    // Request of a clock precision of 1 millisecond if possible.
    const NanoSecond prec = Monotonic::SetPrecision(NanoSecPerMilliSec);
    report.log(2, u"HiDesDevice: get system precision of %'d non-second", {prec});

    TxModeRequest modeRequest;
    TS_ZERO(modeRequest);
    modeRequest.OnOff = 1;
    errno = 0;

    if (::ioctl(fd, IOCTL_ITE_MOD_ENABLETXMODE, &modeRequest) < 0 || modeRequest.error != 0) {
        const int err = errno;
        report.error(u"error enabling transmission: %s", {Guts::HiDesErrorMessage(modeRequest.error, err)});
        return false;
    }

    TxStartTransferRequest startRequest;
    TS_ZERO(startRequest);
    errno = 0;

    if (::ioctl(fd, IOCTL_ITE_MOD_STARTTRANSFER, &startRequest) < 0 || startRequest.error != 0) {
        const int err = errno;
        report.error(u"error starting transmission: %s", {Guts::HiDesErrorMessage(startRequest.error, err)});
        return false;
    }

    transmitting = true;
    pkt_sent = 0;
    all_write = 0;
    fail_write = 0;
    return true;
}


//----------------------------------------------------------------------------
// Stop transmission.
//----------------------------------------------------------------------------

bool ts::HiDesDevice::stopTransmission(Report& report)
{
    if (!_is_open) {
        report.error(u"HiDes device not open");
        return false;
    }
    else {
        return _guts->stopTransmission(report);
    }
}

bool ts::HiDesDevice::Guts::stopTransmission(Report& report)
{
    TxStopTransferRequest stopRequest;
    TS_ZERO(stopRequest);
    errno = 0;

    if (::ioctl(fd, IOCTL_ITE_MOD_STOPTRANSFER, &stopRequest) < 0 || stopRequest.error != 0) {
        const int err = errno;
        report.error(u"error stopping transmission: %s", {Guts::HiDesErrorMessage(stopRequest.error, err)});
        return false;
    }

    TxModeRequest modeRequest;
    TS_ZERO(modeRequest);
    modeRequest.OnOff = 0;
    errno = 0;

    if (::ioctl(fd, IOCTL_ITE_MOD_ENABLETXMODE, &modeRequest) < 0 || modeRequest.error != 0) {
        const int err = errno;
        report.error(u"error disabling transmission: %s", {Guts::HiDesErrorMessage(modeRequest.error, err)});
        return false;
    }

    transmitting = false;
    return true;
}


//----------------------------------------------------------------------------
// Send TS packets.
//----------------------------------------------------------------------------

bool ts::HiDesDevice::send(const TSPacket* packets, size_t packet_count, Report& report)
{
    if (!_is_open) {
        report.error(u"HiDes device not open");
        return false;
    }
    else {
        return _guts->send(packets, packet_count, report);
    }
}

bool ts::HiDesDevice::Guts::send(const TSPacket* packets, size_t packet_count, Report& report)
{
    if (!transmitting) {
        report.error(u"transmission not started");
        return false;
    }

    if (bitrate != 0) {
        if (pkt_sent == 0) {
            // This is the first send operation, initialize timer.
            due_time.getSystemTime();
        }
        else {
            // Check if due time of all previous packets is in the past. In that case, the application
            // was late, we have lost synchronization and we should reset the timer.
            Monotonic now;
            now.getSystemTime();
            if (due_time < now) {
                report.log(2, u"HiDesDevice: late by %'d nano-seconds", {now - due_time});
                due_time = now;
                pkt_sent = 0;
            }
        }
    }

    // Retry several write operations until everything is gone.
    report.log(2, u"HiDesDevice: send %d packets, bitrate = %'d b/s", {packet_count, bitrate});
    const char* data = reinterpret_cast<const char*>(packets);
    size_t remain = packet_count * PKT_SIZE;

    // Normally, we wait before each write operation to be right on time.
    // But, in case we wake up just before the buffer is emptied, we allow
    // a number of short wait timers. These values are arbitrary and may
    // require some tuing in the future.
    const ::useconds_t error_delay = 100;
    const size_t max_retry = 100;
    size_t retry_count = 0;

    while (remain > 0) {

        // Send one burst. Get max burst size.
        const size_t burst = std::min<size_t>(remain, ITE_MAX_SEND_BYTES);

        // If this is the first attempt for this chunk, wait until due time.
        if (retry_count == 0 && bitrate != 0) {
            due_time.wait();
        }

        // Send the chunk.
        // WARNING: write returns an error code, not a size, see comments at beginning of file.
        errno = 0;
        const ssize_t status = ::write(fd, data, burst);
        const int err = errno;

        // Keep statistics on all write operations.
        all_write++;
        if (status != 0) {
            fail_write++;
        }
        report.log(2, u"HiDesDevice:: write = %d, errno = %d, after %d fail (total write: %'d, failed: %'d))", {status, err, retry_count, all_write, fail_write});

        if (status == 0) {
            // Success, assume that the complete burst was sent (ie. written in the buffer in the driver).
            data += burst;
            remain -= burst;
            pkt_sent += burst;
            // Add expected transmission time to our monotonic timer.
            if (bitrate != 0) {
                due_time += (burst * 8 * PKT_SIZE * NanoSecPerSec) / NanoSecond(bitrate);
            }
            // Reset retry count if there are errors in subsequent chunks.
            retry_count = 0;
        }
        else if (errno == EINTR) {
            // Ignore signal, retry
            report.debug(u"HiDesDevice::send: interrupted by signal, retrying");
        }
        else if (retry_count < max_retry) {
            // Short wait and retry same I/O.
            ::usleep(error_delay);
            retry_count++;
        }
        else {
            // Error and no more retry allowed.
            report.error(u"error sending data: %s", {HiDesErrorMessage(status, err)});
            return false;
        }
    }

    return true;
}