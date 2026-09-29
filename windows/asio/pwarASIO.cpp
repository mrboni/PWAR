/*
 * pwarASIO.cpp - PipeWire ASIO Relay (PWAR) project
 *
 * (c) 2025 Philip K. Gisslow
 * This file is part of the PipeWire ASIO Relay (PWAR) project.
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <thread>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <fstream>
#include <sstream>
#include "pwarASIO.h"
#include "pwarASIOLog.h"

#include "../../protocol/pwar_packet.h"
#include "../../protocol/pwar_router.h"
#include "../../protocol/latency_manager.h"

#include <avrt.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "avrt.lib")

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR,12)
#endif

#define PWAR_MAX_CHANNELS 2

static constexpr double TWO_RAISED_TO_32 = 4294967296.0;
static constexpr double TWO_RAISED_TO_32_RECIP = 1.0 / TWO_RAISED_TO_32;

CLSID IID_ASIO_DRIVER = { 0x188135e1, 0xd565, 0x11d2, { 0x85, 0x4f, 0x0, 0xa0, 0xc9, 0x9f, 0x5d, 0x19 } };

CFactoryTemplate g_Templates[1] = {
    {L"PWARASIO", &IID_ASIO_DRIVER, pwarASIO::CreateInstance}
};
int g_cTemplates = sizeof(g_Templates) / sizeof(g_Templates[0]);

CUnknown* pwarASIO::CreateInstance(LPUNKNOWN pUnk, HRESULT* phr) {
    return new pwarASIO(pUnk, phr);
}

STDMETHODIMP pwarASIO::NonDelegatingQueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_ASIO_DRIVER) {
        return GetInterface(this, ppv);
    }
    return CUnknown::NonDelegatingQueryInterface(riid, ppv);
}

extern LONG RegisterAsioDriver(CLSID, char*, char*, char*, char*);
extern LONG UnregisterAsioDriver(CLSID, char*, char*);

HRESULT _stdcall DllRegisterServer() {
    LONG rc = RegisterAsioDriver(IID_ASIO_DRIVER, "PWARASIO.dll", "PWAR ASIO Driver", "PWAR ASIO", "Apartment");
    if (rc) {
        char errstr[128] = {0};
        sprintf(errstr, "Register Server failed ! (%d)", rc);
        MessageBoxA(nullptr, errstr, "ASIO sample Driver", MB_OK);
        return -1;
    }
    return S_OK;
}

HRESULT _stdcall DllUnregisterServer() {
    LONG rc = UnregisterAsioDriver(IID_ASIO_DRIVER, "PWARASIO.dll", "PWAR ASIO Driver");
    if (rc) {
        char errstr[128] = {0};
        sprintf(errstr, "Unregister Server failed ! (%d)", rc);
        MessageBoxA(nullptr, errstr, "ASIO Korg1212 I/O Driver", MB_OK);
        return -1;
    }
    return S_OK;
}

pwarASIO::pwarASIO(LPUNKNOWN pUnk, HRESULT* phr)
    : CUnknown("PWARASIO", pUnk, phr),
      samplePosition(0),
      sampleRate(48000.0),
      callbacks(nullptr),
      blockFrames(kDefaultBlockFrames),
      inputLatency(kDefaultBlockFrames),
      outputLatency(kDefaultBlockFrames * 2),
      activeInputs(0),
      activeOutputs(0),
      toggle(0),
      milliSeconds(static_cast<long>((kDefaultBlockFrames * 1000) / 48000.0)),
      active(false),
      started(false),
      timeInfoMode(false),
      tcRead(false),
      _timestamp(0),
      udpListenerRunning(false),
      udpSendSocket(INVALID_SOCKET),
      udpWSAInitialized(false),
      udpSendIp("192.168.66.2")
{
    // Initialize buffer pointers to null
    for (long i = 0; i < kNumInputs * 2; ++i) {
        inputBuffers[i] = nullptr;
    }
    for (long i = 0; i < kNumOutputs * 2; ++i) {
        outputBuffers[i] = nullptr;
    }
    for (long i = 0; i < kNumInputs; ++i) {
        inMap[i] = 0;
    }
    for (long i = 0; i < kNumOutputs; ++i) {
        outMap[i] = 0;
    }
    
    // Initialize internal buffers to null
    input_buffers = nullptr;
    output_buffers = nullptr;
    
    callbacks = nullptr;
    strcpy(errorMessage, "No error");
    
    parseConfigFile();
    initUdpSender();
}

pwarASIO::~pwarASIO() {
    disposeBuffers();
    closeUdpSender();
}

void pwarASIO::getDriverName(char* name) {
    strcpy(name, "PWAR ASIO Driver");
}

long pwarASIO::getDriverVersion() {
    return 0x00000201L;
}

void pwarASIO::getErrorMessage(char* string) {
    strcpy(string, errorMessage);
}

ASIOBool pwarASIO::init(void* sysRef) {
    (void)sysRef;  // Unused parameter
    if (udpSendSocket == INVALID_SOCKET) {
        strcpy(errorMessage, "Could not initialize the PWAR UDP sender");
        return ASIOFalse;
    }
    
    // Verify that our constants are valid
    if (kMinBlockFrames <= 0 || kMaxBlockFrames <= kMinBlockFrames || 
        kBlockFramesGranularity <= 0 || (kMinBlockFrames % kBlockFramesGranularity) != 0) {
        strcpy(errorMessage, "Invalid buffer size constants");
        return ASIOFalse;
    }
    
    strcpy(errorMessage, "Driver initialized successfully");
    return ASIOTrue;
}

ASIOError pwarASIO::start() {
    if (!callbacks) return ASE_NotPresent;
    if (started) return ASE_OK;
    samplePosition = 0;
    theSystemTime.lo = theSystemTime.hi = 0;
    toggle = 0;
    pwar_router_init(&router, PWAR_MAX_CHANNELS);
    started = true;
    if (!startUdpListener()) {
        started = false;
        return ASE_HWMalfunction;
    }
    return ASE_OK;
}

ASIOError pwarASIO::stop() {
    // Join before the host can release callbacks or buffers. The stop event
    // also wakes an idle receiver, without depending on a packet from the Pi.
    stopUdpListener();
    started = false;
    return ASE_OK;
}

ASIOError pwarASIO::getChannels(long* numInputChannels, long* numOutputChannels) {
    if (!numInputChannels || !numOutputChannels) {
        strcpy(errorMessage, "Null pointer passed to getChannels");
        return ASE_InvalidParameter;
    }
    *numInputChannels = kNumInputs;
    *numOutputChannels = kNumOutputs;
    return ASE_OK;
}

ASIOError pwarASIO::getLatencies(long* _inputLatency, long* _outputLatency) {
    *_inputLatency = blockFrames;
    *_outputLatency = blockFrames * 2;
    return ASE_OK;
}

ASIOError pwarASIO::getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) {
    if (!minSize || !maxSize || !preferredSize || !granularity) {
        strcpy(errorMessage, "Null pointer passed to getBufferSize");
        return ASE_InvalidParameter;
    }
    *minSize = kMinBlockFrames;
    *maxSize = kMaxBlockFrames;
    *preferredSize = kDefaultBlockFrames;
    *granularity = kBlockFramesGranularity;
    return ASE_OK;
}

ASIOError pwarASIO::canSampleRate(ASIOSampleRate sampleRate) {
    return (sampleRate == 44100.0 || sampleRate == 48000.0) ? ASE_OK : ASE_NoClock;
}

ASIOError pwarASIO::getSampleRate(ASIOSampleRate* sampleRate) {
    *sampleRate = this->sampleRate;
    return ASE_OK;
}

ASIOError pwarASIO::setSampleRate(ASIOSampleRate sampleRate) {
    if (sampleRate != 44100.0 && sampleRate != 48000.0) return ASE_NoClock;
    if (started && sampleRate != this->sampleRate) return ASE_InvalidMode;
    if (sampleRate != this->sampleRate) {
        this->sampleRate = sampleRate;
        asioTime.timeInfo.sampleRate = sampleRate;
        asioTime.timeInfo.flags |= kSampleRateChanged;
        milliSeconds = static_cast<long>((blockFrames * 1000) / this->sampleRate);
        if (callbacks && callbacks->sampleRateDidChange)
            callbacks->sampleRateDidChange(this->sampleRate);
    }
    return ASE_OK;
}

ASIOError pwarASIO::getClockSources(ASIOClockSource* clocks, long* numSources) {
    clocks->index = 0;
    clocks->associatedChannel = -1;
    clocks->associatedGroup = -1;
    clocks->isCurrentSource = ASIOTrue;
    strcpy(clocks->name, "Internal");
    *numSources = 1;
    return ASE_OK;
}

ASIOError pwarASIO::setClockSource(long index) {
    if (index == 0) {
        asioTime.timeInfo.flags |= kClockSourceChanged;
        return ASE_OK;
    }
    return ASE_NotPresent;
}

ASIOError pwarASIO::getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) {
    tStamp->lo = theSystemTime.lo;
    tStamp->hi = theSystemTime.hi;
    if (samplePosition >= TWO_RAISED_TO_32) {
        sPos->hi = static_cast<unsigned long>(samplePosition * TWO_RAISED_TO_32_RECIP);
        sPos->lo = static_cast<unsigned long>(samplePosition - (sPos->hi * TWO_RAISED_TO_32));
    } else {
        sPos->hi = 0;
        sPos->lo = static_cast<unsigned long>(samplePosition);
    }
    return ASE_OK;
}

ASIOError pwarASIO::getChannelInfo(ASIOChannelInfo* info) {
    if (!info) {
        strcpy(errorMessage, "Null pointer passed to getChannelInfo");
        return ASE_InvalidParameter;
    }
    
    if (info->channel < 0 || (info->isInput ? info->channel >= kNumInputs : info->channel >= kNumOutputs)) {
        strcpy(errorMessage, "Channel index out of range");
        return ASE_InvalidParameter;
    }
    
    info->type = ASIOSTFloat32LSB;
    info->channelGroup = 0;
    info->isActive = ASIOFalse;
    
    if (info->isInput) {
        sprintf(info->name, "Input %ld", info->channel + 1);
        for (long i = 0; i < activeInputs; ++i) {
            if (inMap[i] == info->channel) {
                info->isActive = ASIOTrue;
                break;
            }
        }
    } else {
        sprintf(info->name, "Output %ld", info->channel + 1);
        for (long i = 0; i < activeOutputs; ++i) {
            if (outMap[i] == info->channel) {
                info->isActive = ASIOTrue;
                break;
            }
        }
    }
    return ASE_OK;
}

ASIOError pwarASIO::createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) {
    if (!bufferInfos || !callbacks) {
        strcpy(errorMessage, "Null pointer passed to createBuffers");
        return ASE_InvalidParameter;
    }
    
    if (numChannels <= 0 || numChannels > (kNumInputs + kNumOutputs)) {
        strcpy(errorMessage, "Invalid number of channels");
        return ASE_InvalidParameter;
    }
    
    ASIOBufferInfo* info = bufferInfos;
    bool notEnoughMem = false;
    
    // Validate buffer size
    if (bufferSize < kMinBlockFrames || bufferSize > kMaxBlockFrames) {
        strcpy(errorMessage, "Buffer size out of supported range");
        return ASE_InvalidParameter;
    }
    
    // Check if buffer size is aligned to granularity
    if ((bufferSize % kBlockFramesGranularity) != 0) {
        strcpy(errorMessage, "Buffer size not aligned to granularity");
        return ASE_InvalidParameter;
    }
    
    // Reject duplicate channels before indexing the per-channel arrays.
    bool seenInputs[kNumInputs] = {};
    bool seenOutputs[kNumOutputs] = {};
    for (long i = 0; i < numChannels; ++i) {
        const auto& channel = bufferInfos[i];
        const long limit = channel.isInput ? kNumInputs : kNumOutputs;
        bool* seen = channel.isInput ? seenInputs : seenOutputs;
        if (channel.channelNum < 0 || channel.channelNum >= limit || seen[channel.channelNum])
            return ASE_InvalidParameter;
        seen[channel.channelNum] = true;
    }
    disposeBuffers();
    blockFrames = bufferSize;
    
    // Update latencies and timing based on new buffer size
    inputLatency = blockFrames;
    outputLatency = blockFrames * 2;
    milliSeconds = static_cast<long>((blockFrames * 1000) / sampleRate);
    
    for (long i = 0; i < numChannels; ++i, ++info) {
        if (info->isInput) {
            if (info->channelNum < 0 || info->channelNum >= kNumInputs)
                goto error;
            inMap[activeInputs] = info->channelNum;
            inputBuffers[activeInputs] = new float[blockFrames * 2]();
            if (inputBuffers[activeInputs]) {
                info->buffers[0] = inputBuffers[activeInputs];
                info->buffers[1] = inputBuffers[activeInputs] + blockFrames;
            } else {
                info->buffers[0] = info->buffers[1] = nullptr;
                notEnoughMem = true;
            }
            ++activeInputs;
            if (activeInputs > kNumInputs) {
error:
                disposeBuffers();
                return ASE_InvalidParameter;
            }
        } else {
            if (info->channelNum < 0 || info->channelNum >= kNumOutputs)
                goto error;
            outMap[activeOutputs] = info->channelNum;
            outputBuffers[activeOutputs] = new float[blockFrames * 2]();
            if (outputBuffers[activeOutputs]) {
                info->buffers[0] = outputBuffers[activeOutputs];
                info->buffers[1] = outputBuffers[activeOutputs] + blockFrames;
            } else {
                info->buffers[0] = info->buffers[1] = nullptr;
                notEnoughMem = true;
            }
            ++activeOutputs;
            if (activeOutputs > kNumOutputs) {
                --activeOutputs;
                disposeBuffers();
                return ASE_InvalidParameter;
            }
        }
    }
    if (notEnoughMem) {
        disposeBuffers();
        return ASE_NoMemory;
    }
    this->callbacks = callbacks;
    // Initialize the router with the number of output channels
    pwar_router_init(&router, PWAR_MAX_CHANNELS);
    input_buffers = new float[PWAR_MAX_CHANNELS * blockFrames]();
    output_buffers = new float[PWAR_MAX_CHANNELS * blockFrames]();

    if (callbacks->asioMessage && callbacks->bufferSwitchTimeInfo &&
        callbacks->asioMessage(kAsioSupportsTimeInfo, 0, 0, 0)) {
        timeInfoMode = true;
        asioTime.timeInfo.speed = 1.0;
        asioTime.timeInfo.systemTime.hi = asioTime.timeInfo.systemTime.lo = 0;
        asioTime.timeInfo.samplePosition.hi = asioTime.timeInfo.samplePosition.lo = 0;
        asioTime.timeInfo.sampleRate = sampleRate;
        asioTime.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;
        asioTime.timeCode.speed = 1.0;
        asioTime.timeCode.timeCodeSamples.lo = asioTime.timeCode.timeCodeSamples.hi = 0;
        asioTime.timeCode.flags = kTcValid | kTcRunning;
    } else {
        timeInfoMode = false;
        if (!callbacks->bufferSwitch) {
            disposeBuffers();
            return ASE_InvalidParameter;
        }
    }
    return ASE_OK;
}

ASIOError pwarASIO::disposeBuffers() {
    // Stop audio processing first
    stop();
    
    // Clear callbacks to prevent any more audio callbacks
    callbacks = nullptr;
    
    // Clean up input buffers
    for (long i = 0; i < activeInputs; ++i) {
        if (inputBuffers[i]) {
            delete[] inputBuffers[i];
            inputBuffers[i] = nullptr;
        }
    }
    activeInputs = 0;
    
    // Clean up output buffers
    for (long i = 0; i < activeOutputs; ++i) {
        if (outputBuffers[i]) {
            delete[] outputBuffers[i];
            outputBuffers[i] = nullptr;
        }
    }
    activeOutputs = 0;
    
    // Clean up internal buffers
    if (input_buffers) {
        delete[] input_buffers;
        input_buffers = nullptr;
    }
    if (output_buffers) {
        delete[] output_buffers;
        output_buffers = nullptr;
    }
    
    return ASE_OK;
}

ASIOError pwarASIO::controlPanel() {
    // For now, we don't provide a control panel
    // This could be extended to show buffer size settings
    char msg[256];
    sprintf(msg, "PWAR ASIO Driver v%d.%d.%d\n\n"
                 "Current buffer size: %ld samples\n"
                 "Supported range: %d - %d samples\n"
                 "Granularity: %d samples\n\n"
                 "Buffer size can be changed through your DAW's audio settings.",
                 (getDriverVersion() >> 16) & 0xFF,
                 (getDriverVersion() >> 8) & 0xFF,
                 getDriverVersion() & 0xFF,
                 blockFrames,
                 kMinBlockFrames,
                 kMaxBlockFrames,
                 kBlockFramesGranularity);
    
    MessageBoxA(nullptr, msg, "PWAR ASIO Driver", MB_OK | MB_ICONINFORMATION);
    return ASE_OK;
}

ASIOError pwarASIO::future(long selector, void* opt) {
    switch (selector) {
        case kAsioEnableTimeCodeRead: tcRead = true; return ASE_SUCCESS;
        case kAsioDisableTimeCodeRead: tcRead = false; return ASE_SUCCESS;
        case kAsioSetInputMonitor: return ASE_SUCCESS;
        case kAsioCanInputMonitor: return ASE_SUCCESS;
        case kAsioCanTimeInfo: return ASE_SUCCESS;
        case kAsioCanTimeCode: return ASE_SUCCESS;
    }
    return ASE_NotPresent;
}

void pwarASIO::output(const pwar_packet_t& packet) {
    if (udpSendSocket != INVALID_SOCKET) {
        WSABUF buffer;
        buffer.buf = reinterpret_cast<CHAR*>(const_cast<pwar_packet_t*>(&packet));
        buffer.len = sizeof(pwar_packet_t);
        DWORD bytesSent = 0;
        int flags = 0;
        WSASendTo(udpSendSocket, &buffer, 1, &bytesSent, flags,
                  reinterpret_cast<sockaddr*>(&udpSendAddr), sizeof(udpSendAddr), NULL, NULL);
    }
}

void pwarASIO::bufferSwitchX() {
    getSamplePosition(&asioTime.timeInfo.samplePosition, &asioTime.timeInfo.systemTime);
    if (tcRead) {
        asioTime.timeCode.timeCodeSamples.lo = asioTime.timeInfo.samplePosition.lo + static_cast<unsigned long>(600.0 * sampleRate);
        asioTime.timeCode.timeCodeSamples.hi = 0;
    }
    callbacks->bufferSwitchTimeInfo(&asioTime, toggle, ASIOFalse);
    asioTime.timeInfo.flags &= ~(kSampleRateChanged | kClockSourceChanged);
}

ASIOError pwarASIO::outputReady() {
    return ASE_NotPresent;
}

void pwarASIO::processAudioBlock(bool silence) {
    for (long i = 0; i < activeInputs; ++i) {
        float* dest = inputBuffers[i] + (toggle ? blockFrames : 0);
        if (silence) {
            memset(dest, 0, blockFrames * sizeof(float));
        } else {
            memcpy(dest, input_buffers + inMap[i] * blockFrames, blockFrames * sizeof(float));
        }
    }
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    theSystemTime.lo = static_cast<unsigned long>(ns);
    theSystemTime.hi = static_cast<unsigned long>(static_cast<uint64_t>(ns) >> 32);
    samplePosition += blockFrames;
    if (timeInfoMode) {
        bufferSwitchX();
    } else {
        callbacks->bufferSwitch(toggle, ASIOFalse);
    }
    // Hosts may enable only one output, reverse channel order, or use inputs only.
    memset(output_buffers, 0, PWAR_MAX_CHANNELS * blockFrames * sizeof(float));
    for (long i = 0; i < activeOutputs; ++i) {
        memcpy(output_buffers + outMap[i] * blockFrames,
               outputBuffers[i] + (toggle ? blockFrames : 0), blockFrames * sizeof(float));
    }
    toggle = toggle ? 0 : 1;
}

void pwarASIO::udp_packet_listener() {
    DWORD mmcssTaskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsA("Pro Audio", &mmcssTaskIndex);
    const bool timerResolutionSet = timeBeginPeriod(1) == TIMERR_NOERROR;
    using Clock = std::chrono::steady_clock;
    const auto blockPeriod = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(blockFrames / sampleRate));
    // Healthy streams remain entirely paced by the Pi. Only fall back after
    // 100 ms without a complete buffer, so packet jitter does not add callbacks.
    const auto networkGrace = std::chrono::milliseconds(100);
    auto nextCallback = Clock::now();
    bool offline = true;
    WSAEVENT events[] = {udpStopEvent, udpReceiveEvent};
    pwar_packet_t outputPackets[32];

    while (udpListenerRunning.load()) {
        auto remaining = nextCallback - Clock::now();
        const DWORD timeout = remaining <= Clock::duration::zero() ? 0 :
            static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count() + 1);
        const DWORD wait = WSAWaitForMultipleEvents(2, events, FALSE, timeout, FALSE);
        if (!udpListenerRunning.load() || wait == WSA_WAIT_EVENT_0) break;
        if (wait == WSA_WAIT_FAILED) break;

        if (wait == WSA_WAIT_EVENT_0 + 1) {
            WSANETWORKEVENTS networkEvents{};
            WSAEnumNetworkEvents(udpReceiveSocket, udpReceiveEvent, &networkEvents);
            if (networkEvents.lNetworkEvents & FD_READ) {
                pwar_packet_t packet{};
                // The socket is nonblocking after WSAEventSelect. One receive
                // re-arms FD_READ, including when more datagrams are queued.
                const int received = recvfrom(udpReceiveSocket, reinterpret_cast<char*>(&packet),
                                              sizeof(packet), 0, nullptr, nullptr);
                const uint32_t chunkSize = packet.n_samples;
                if (received == sizeof(packet) && chunkSize >= PWAR_PACKET_MIN_CHUNK_SIZE &&
                    chunkSize <= PWAR_PACKET_MAX_CHUNK_SIZE && blockFrames % chunkSize == 0 &&
                    packet.packet_index < static_cast<uint32_t>(blockFrames) / chunkSize &&
                    udpListenerRunning.load()) {
                    packet.num_packets = blockFrames / chunkSize;
                    latency_manager_process_packet_client(&packet);
                    const int samplesReady = pwar_router_process_streaming_packet(
                        &router, &packet, input_buffers, blockFrames, PWAR_MAX_CHANNELS);
                    if (samplesReady > 0) {
                        latency_manager_start_audio_cbk_begin();
                        processAudioBlock(false);
                        latency_manager_start_audio_cbk_end();
                        offline = false;
                        nextCallback = Clock::now() + networkGrace;

                        if (activeOutputs > 0) {
                            uint32_t packetsToSend = 0;
                            pwar_router_send_buffer(&router, chunkSize, output_buffers, samplesReady,
                                                    PWAR_MAX_CHANNELS, outputPackets, 32, &packetsToSend);
                            const uint64_t timestamp = latency_manager_timestamp_now();
                            for (uint32_t i = 0; i < packetsToSend; ++i) {
                                outputPackets[i].seq = packet.seq;
                                outputPackets[i].timestamp = timestamp;
                                output(outputPackets[i]);
                            }
                            pwar_latency_info_t latencyInfo;
                            if (latency_manager_time_for_sending_latency_info(&latencyInfo)) {
                                sendto(udpSendSocket, reinterpret_cast<const char*>(&latencyInfo),
                                       sizeof(latencyInfo), 0, reinterpret_cast<sockaddr*>(&udpSendAddr),
                                       sizeof(udpSendAddr));
                            }
                        }
                    }
                }
            }
        }
        // Check the deadline even under malformed/incomplete packet traffic.
        // All callbacks use this one thread, so fallback and network callbacks
        // can never run concurrently.
        if (udpListenerRunning.load() && Clock::now() >= nextCallback) {
            if (!offline) pwar_router_init(&router, PWAR_MAX_CHANNELS);
            offline = true;
            processAudioBlock(true);
            nextCallback += blockPeriod;
            if (nextCallback < Clock::now()) nextCallback = Clock::now() + blockPeriod;
        }
    }
    if (timerResolutionSet) timeEndPeriod(1);
    if (mmcssHandle) AvRevertMmThreadCharacteristics(mmcssHandle);
}

bool pwarASIO::startUdpListener() {
    udpStopEvent = WSACreateEvent();
    udpReceiveEvent = WSACreateEvent();
    udpReceiveSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpStopEvent == WSA_INVALID_EVENT || udpReceiveEvent == WSA_INVALID_EVENT ||
        udpReceiveSocket == INVALID_SOCKET) {
        strcpy(errorMessage, "Could not create PWAR receive socket/events");
        stopUdpListener();
        return false;
    }
    BOOL exclusive = TRUE;
    setsockopt(udpReceiveSocket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    int receiveBufferSize = 1024;
    setsockopt(udpReceiveSocket, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&receiveBufferSize), sizeof(receiveBufferSize));
    DWORD bytesReturned = 0;
    BOOL connectionReset = FALSE;
    WSAIoctl(udpReceiveSocket, SIO_UDP_CONNRESET, &connectionReset, sizeof(connectionReset),
             nullptr, 0, &bytesReturned, nullptr, nullptr);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(8321);
    if (bind(udpReceiveSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        WSAEventSelect(udpReceiveSocket, udpReceiveEvent, FD_READ) == SOCKET_ERROR) {
        sprintf(errorMessage, "Could not listen on UDP 8321 (Winsock error %d)", WSAGetLastError());
        stopUdpListener();
        return false;
    }
    udpListenerRunning.store(true);
    try {
        udpListenerThread = std::thread(&pwarASIO::udp_packet_listener, this);
    } catch (const std::exception&) {
        strcpy(errorMessage, "Could not start PWAR receive thread");
        stopUdpListener();
        return false;
    }
    return true;
}

void pwarASIO::stopUdpListener() {
    udpListenerRunning.store(false);
    if (udpStopEvent != WSA_INVALID_EVENT) WSASetEvent(udpStopEvent);
    if (udpListenerThread.joinable()) udpListenerThread.join();
    // Nothing can receive, send or invoke the host after the join. Only now
    // close handles and let stop()/disposeBuffers() return to the host.
    if (udpReceiveSocket != INVALID_SOCKET) {
        closesocket(udpReceiveSocket);
        udpReceiveSocket = INVALID_SOCKET;
    }
    if (udpReceiveEvent != WSA_INVALID_EVENT) {
        WSACloseEvent(udpReceiveEvent);
        udpReceiveEvent = WSA_INVALID_EVENT;
    }
    if (udpStopEvent != WSA_INVALID_EVENT) {
        WSACloseEvent(udpStopEvent);
        udpStopEvent = WSA_INVALID_EVENT;
    }
}

void pwarASIO::parseConfigFile() {
    std::string configPath;
    const char* home = getenv("USERPROFILE");
    if (home && *home) {
        configPath = std::string(home) + "\\pwarASIO.cfg";
    } else {
        configPath = "pwarASIO.cfg";
    }
    std::ifstream cfg(configPath);
    if (!cfg.is_open()) {
        return;
    }
    std::string line;
    while (std::getline(cfg, line)) {
        std::istringstream iss(line);
        std::string key, value;
        if (std::getline(iss, key, '=') && std::getline(iss, value)) {
            if (key == "udp_send_ip") {
                udpSendIp = value;
                pwarASIOLog::Send("Read ip from config");
            }
        }
    }
}

void pwarASIO::initUdpSender() {
    constexpr int udp_port = 8321;
    if (!udpWSAInitialized) {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2,2), &wsaData) == 0) {
            udpWSAInitialized = true;
        } else {
            pwarASIOLog::Send("WSAStartup failed in initUdpSender");
            return;
        }
    }
    if (udpSendSocket == INVALID_SOCKET && udpWSAInitialized) {
        udpSendSocket = socket(AF_INET, SOCK_DGRAM, 0);
        if (udpSendSocket != INVALID_SOCKET) {
            // A congested/disconnected peer must not hold up the worker's
            // shutdown while it is sending the last audio buffer.
            u_long nonblocking = 1;
            if (ioctlsocket(udpSendSocket, FIONBIO, &nonblocking) == SOCKET_ERROR) {
                closesocket(udpSendSocket);
                udpSendSocket = INVALID_SOCKET;
                return;
            }
            memset(&udpSendAddr, 0, sizeof(udpSendAddr));
            udpSendAddr.sin_family = AF_INET;
            udpSendAddr.sin_port = htons(udp_port);
            if (inet_pton(AF_INET, udpSendIp.c_str(), &udpSendAddr.sin_addr) != 1) {
                closesocket(udpSendSocket);
                udpSendSocket = INVALID_SOCKET;
                return;
            }
            // Set SO_SNDBUF to minimal size for low latency
            int sndbuf = 1024; // 1 KB
            setsockopt(udpSendSocket, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof(sndbuf));
            // Disable UDP connection reset behavior
            DWORD bytesReturned = 0;
            BOOL bNewBehavior = FALSE;
            WSAIoctl(udpSendSocket, SIO_UDP_CONNRESET, &bNewBehavior, sizeof(bNewBehavior),
                     NULL, 0, &bytesReturned, NULL, NULL);
        } else {
            pwarASIOLog::Send("Failed to create UDP send socket");
        }
    }
}

void pwarASIO::closeUdpSender() {
    if (udpSendSocket != INVALID_SOCKET) {
        closesocket(udpSendSocket);
        udpSendSocket = INVALID_SOCKET;
    }
    if (udpWSAInitialized) {
        WSACleanup();
        udpWSAInitialized = false;
    }
}

// Dynamic buffer size support methods
ASIOError pwarASIO::setBufferSize(long newBufferSize) {
    if (!isValidBufferSize(newBufferSize)) {
        strcpy(errorMessage, "Invalid buffer size");
        return ASE_InvalidParameter;
    }
    
    // Can't change buffer size while active
    if (started) {
        strcpy(errorMessage, "Cannot change buffer size while driver is active");
        return ASE_NotPresent;  // Use standard ASIO error code
    }
    
    blockFrames = newBufferSize;
    inputLatency = blockFrames;
    outputLatency = blockFrames * 2;
    milliSeconds = static_cast<long>((blockFrames * 1000) / sampleRate);
    
    return ASE_OK;
}

bool pwarASIO::isValidBufferSize(long bufferSize) const {
    return (bufferSize >= kMinBlockFrames && 
            bufferSize <= kMaxBlockFrames && 
            (bufferSize % kBlockFramesGranularity) == 0);
}
