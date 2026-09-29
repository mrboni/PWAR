// Exercise the built DLL through its public COM/ASIO interface, without registration.
#include <winsock2.h>
#include <windows.h>
#include <objbase.h>
#include "iasiodrv.h"
#include "../../../protocol/pwar_packet.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
static const CLSID driverId = {0x188135e1, 0xd565, 0x11d2, {0x85,0x4f,0,0xa0,0xc9,0x9f,0x5d,0x19}};
static std::atomic<unsigned> callbacks{0}, silentBlocks{0}, networkBlocks{0}, callbackErrors{0};
static float* inputBuffers[2]{};
static long blockFrames = 128;
static bool useTimeInfo = true;
static uint64_t previousPosition = 0, previousTimestamp = 0;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static void pauseMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static void bufferSwitch(long index, ASIOBool) {
    if (index < 0 || index > 1) { ++callbackErrors; return; }
    if (inputBuffers[index]) {
        const float sample = inputBuffers[index][0];
        if (sample == 0.0f) ++silentBlocks;
        else if (sample == 0.25f) ++networkBlocks;
        else ++callbackErrors;
        for (long i = 1; i < blockFrames; ++i)
            if (inputBuffers[index][i] != sample) ++callbackErrors;
    }
    ++callbacks;
}
static ASIOTime* bufferSwitchTimeInfo(ASIOTime* time, long index, ASIOBool direct) {
    const uint64_t position = (uint64_t(time->timeInfo.samplePosition.hi) << 32) | time->timeInfo.samplePosition.lo;
    const uint64_t timestamp = (uint64_t(time->timeInfo.systemTime.hi) << 32) | time->timeInfo.systemTime.lo;
    if (position != previousPosition + blockFrames || timestamp <= previousTimestamp) ++callbackErrors;
    previousPosition = position;
    previousTimestamp = timestamp;
    bufferSwitch(index, direct);
    return time;
}
static void sampleRateChanged(ASIOSampleRate) {}
static long asioMessage(long selector, long, void*, double*) {
    return selector == kAsioSupportsTimeInfo && useTimeInfo ? 1 : 0;
}
static ASIOCallbacks hostCallbacks{bufferSwitch, sampleRateChanged, asioMessage, bufferSwitchTimeInfo};

struct Driver {
    IASIO* asio = nullptr;
    std::vector<ASIOBufferInfo> buffers;
    explicit Driver(IClassFactory* factory) {
        require(SUCCEEDED(factory->CreateInstance(nullptr, driverId, reinterpret_cast<void**>(&asio))), "CreateInstance failed");
        require(asio->init(nullptr) != ASIOFalse, "ASIO init failed");
    }
    ~Driver() { if (asio) asio->Release(); }
    void configure(long frames = 128, bool outputs = false, double rate = 48000) {
        require(asio->stop() == ASE_OK, "Stop before configuration failed");
        require(asio->setSampleRate(rate) == ASE_OK, "setSampleRate failed");
        blockFrames = frames;
        buffers.clear();
        ASIOBufferInfo input{};
        input.isInput = ASIOTrue;
        input.channelNum = 0;
        buffers.push_back(input);
        if (outputs) {
            // Request outputs in reverse order to exercise channel mapping.
            for (long channel : {1L, 0L}) {
                ASIOBufferInfo output{};
                output.isInput = ASIOFalse;
                output.channelNum = channel;
                buffers.push_back(output);
            }
        }
        require(asio->createBuffers(buffers.data(), static_cast<long>(buffers.size()), frames, &hostCallbacks) == ASE_OK,
                "createBuffers failed");
        for (int i = 0; i < 2; ++i) inputBuffers[i] = static_cast<float*>(buffers[0].buffers[i]);
    }
    void start() {
        callbacks = silentBlocks = networkBlocks = callbackErrors = 0;
        previousPosition = previousTimestamp = 0;
        require(asio->start() == ASE_OK, "ASIO start failed");
    }
    void stop() {
        const auto begin = Clock::now();
        require(asio->stop() == ASE_OK, "ASIO stop failed");
        require(Clock::now() - begin < std::chrono::milliseconds(500), "Stop exceeded 500 ms");
        const auto count = callbacks.load();
        pauseMs(10);
        require(callbacks.load() == count, "Callback occurred after stop returned");
        require(callbackErrors.load() == 0, "Invalid samples, position or timestamp in callback");
    }
};

struct Sender {
    SOCKET socketHandle = INVALID_SOCKET;
    sockaddr_in destination{};
    Sender() {
        socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        require(socketHandle != INVALID_SOCKET, "Test socket creation failed");
        destination.sin_family = AF_INET;
        destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        destination.sin_port = htons(8321);
    }
    ~Sender() { closesocket(socketHandle); }
    void packet(uint64_t sequence, uint16_t samples = 128) {
        pwar_packet_t data{};
        data.n_samples = samples;
        data.seq = sequence;
        data.num_packets = 1;
        for (auto& channel : data.samples) for (float& sample : channel) sample = 0.25f;
        require(sendto(socketHandle, reinterpret_cast<const char*>(&data), sizeof(data), 0,
                       reinterpret_cast<sockaddr*>(&destination), sizeof(destination)) == sizeof(data), "Test send failed");
    }
};

static void testOffline(IClassFactory* factory) {
    for (bool timeInfo : {true, false}) {
        useTimeInfo = timeInfo;
        for (long frames : {64L, 128L, 2048L}) {
            Driver driver(factory);
            driver.configure(frames, true, timeInfo ? 48000 : 44100);
            driver.start();
            pauseMs(220);
            driver.stop();
            require(callbacks >= 3 && callbacks < 1000, "Missing or runaway offline callbacks");
            require(silentBlocks == callbacks, "Offline input was not silent");
        }
    }
}

static void testReceive(IClassFactory* factory) {
    Driver driver(factory);
    // Input-only mode needs no return stream and keeps all test traffic local.
    driver.configure();
    driver.start();
    Sender sender;
    for (unsigned i = 0; i < 16; ++i) { sender.packet(100 + i); pauseMs(5); }
    require(networkBlocks >= 8, "Complete network packets did not drive callbacks");
    const auto silenceBefore = silentBlocks.load();
    pauseMs(180);
    require(silentBlocks > silenceBefore + 3, "No fallback callbacks after peer disappeared");
    const auto receivedBefore = networkBlocks.load();
    for (unsigned i = 0; i < 10; ++i) { sender.packet(1000 + i); pauseMs(5); }
    require(networkBlocks >= receivedBefore + 5, "Network callbacks did not recover");
    const auto beforeInvalid = silentBlocks.load();
    for (unsigned i = 0; i < 40; ++i) {
        // Reject zero/oversized chunks and keep incomplete sequences from
        // starving the independent fallback deadline.
        sender.packet(2000 + i * 10, i % 3 == 0 ? 0 : i % 3 == 1 ? 65535 : 64);
        pauseMs(5);
    }
    require(silentBlocks > beforeInvalid + 3, "Malformed/partial packets starved fallback");
    std::atomic<bool> sending{true};
    std::thread producer([&] {
        for (uint64_t seq = 10000; sending; ++seq) { sender.packet(seq); pauseMs(1); }
    });
    pauseMs(20);
    driver.stop();
    sending = false;
    producer.join();
}

static void testLifecycle(IClassFactory* factory) {
    // Initialize any DLL-wide state before counting handles.
    { Driver warmup(factory); }
    DWORD initialHandles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &initialHandles);
    for (int iteration = 0; iteration < 50; ++iteration) {
        Driver driver(factory);
        require(driver.asio->start() == ASE_NotPresent, "Start accepted missing buffers");
        if (iteration % 5 == 0) continue; // create/release without ever starting
        driver.configure(iteration % 2 ? 128 : 192);
        driver.start();
        pauseMs(3);
        driver.stop();
        driver.start();
        pauseMs(3);
        const auto begin = Clock::now();
        require(driver.asio->disposeBuffers() == ASE_OK, "Active disposeBuffers failed");
        require(Clock::now() - begin < std::chrono::milliseconds(500), "Active disposal exceeded 500 ms");
        const auto count = callbacks.load();
        pauseMs(3);
        require(callbacks.load() == count, "Callback occurred after buffer disposal");
        ASIOBufferInfo duplicates[2]{};
        duplicates[0].isInput = duplicates[1].isInput = ASIOTrue;
        require(driver.asio->createBuffers(duplicates, 2, 128, &hostCallbacks) == ASE_InvalidParameter,
                "Duplicate channels accepted");
        driver.configure();
        driver.start();
        pauseMs(2);
        // Destructor must quiesce the worker even without an explicit stop.
    }
    DWORD finalHandles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &finalHandles);
    require(finalHandles <= initialHandles + 3, "Driver lifecycle leaked Windows handles");
}

static void testBindConflict(IClassFactory* factory) {
    Sender blocker;
    BOOL exclusive = TRUE;
    setsockopt(blocker.socketHandle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<char*>(&exclusive), sizeof(exclusive));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(8321);
    require(bind(blocker.socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "Could not reserve test port; close other PWAR hosts before testing");
    Driver driver(factory);
    driver.configure();
    const auto begin = Clock::now();
    require(driver.asio->start() == ASE_HWMalfunction, "Busy port did not fail startup");
    require(Clock::now() - begin < std::chrono::milliseconds(500), "Busy-port startup stalled");
    char error[256]{};
    driver.asio->getErrorMessage(error);
    require(std::strstr(error, "8321") != nullptr, "Missing useful bind error");
    closesocket(blocker.socketHandle);
    blocker.socketHandle = INVALID_SOCKET;
    driver.start();
    pauseMs(30);
    driver.stop();
    require(callbacks > 0, "Failed start could not be retried");
}

int main(int argc, char** argv) {
    try {
        require(argc == 3, "Usage: lifecycle-test DLL scenario");
        WSADATA data;
        require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "WSAStartup failed");
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "COM initialization failed");
        HMODULE dll = nullptr;
        IClassFactory* factory = nullptr;
        if (std::string(argv[1]) == "--registered") {
            require(SUCCEEDED(CoGetClassObject(driverId, CLSCTX_INPROC_SERVER, nullptr, IID_IClassFactory,
                                               reinterpret_cast<void**>(&factory))), "Registered factory lookup failed");
            char loadedPath[MAX_PATH]{};
            GetModuleFileNameA(GetModuleHandleA("PWARASIO.dll"), loadedPath, MAX_PATH);
            std::cout << "Registered DLL: " << loadedPath << '\n';
        } else {
            dll = LoadLibraryA(argv[1]);
            require(dll != nullptr, "LoadLibrary failed");
            auto getClassObject = reinterpret_cast<HRESULT (STDAPICALLTYPE*)(REFCLSID, REFIID, void**)>(
                GetProcAddress(dll, "DllGetClassObject"));
            require(getClassObject != nullptr, "DllGetClassObject export missing");
            require(SUCCEEDED(getClassObject(driverId, IID_IClassFactory, reinterpret_cast<void**>(&factory))), "Factory lookup failed");
        }
        const std::string scenario = argv[2];
        if (scenario == "offline") testOffline(factory);
        else if (scenario == "receive") testReceive(factory);
        else if (scenario == "lifecycle") testLifecycle(factory);
        else if (scenario == "bind-conflict") testBindConflict(factory);
        else throw std::runtime_error("Unknown scenario");
        factory->Release();
        if (dll) FreeLibrary(dll);
        CoUninitialize();
        WSACleanup();
        std::cout << "PASS " << scenario << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
