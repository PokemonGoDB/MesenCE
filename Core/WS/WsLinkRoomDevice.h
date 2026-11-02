#pragma once
#include "pch.h"
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <deque>
#include "WS/WsSerialDevice.h"

//Link play: the console's serial line joined to another player's console through a room on a server
//(swanrevival.com/link-play/), as if by Bandai's link cable. The UI holds the room's WebSocket (LinkRoomRelay);
//this device connects to it on a local port and swaps framed messages with it:
//  device → UI  'L' 'K' 1                     once, on connecting
//  both ways    [type][length, 2 bytes LE][payload]
//    UI → device  'S' start: [flags: bit 0 keep the newer byte on overrun, bit 1 38400 baud][jitter allowance, ms]
//    UI → device  'X' stop (back to silent)
//    both ways    'D' link data: 6-byte records, as in the room (time in µs since the sender's start, LE u32;
//                     the byte; flags, bit 0 = 38400 baud)
//Until the start the device is silent, as if no cable were plugged in. After it, each byte the console sends is
//stamped with the emulated clock, and the partner's bytes are played to the console with their original spacing:
//a jitter buffer on the emulated clock (the same rules as the site's src/lib/link-records.ts).
class WsLinkRoomDevice final : public IWsSerialDevice
{
private:
	struct Record
	{
		uint32_t Time; //µs
		uint8_t Value;
		bool Fast;
	};
	struct Due
	{
		uint64_t Clock;
		uint8_t Value;
	};

	uint16_t _relayPort = 0;
	std::thread _thread;
	std::atomic<bool> _stop = false;

	//Shared with the network thread
	std::mutex _lock;
	std::deque<Record> _fromRoom;
	vector<uint8_t> _toRoom;
	std::chrono::steady_clock::time_point _toRoomSince;
	std::atomic<int> _command = 0; //1 start, 2 stop: picked up on the emulation thread
	std::atomic<bool> _keepNewer = false;
	std::atomic<bool> _fast = false;
	std::atomic<uint32_t> _jitterMs = 10;

	//Emulation thread only
	bool _started = false;
	uint64_t _startClock = 0;
	std::deque<Due> _due;
	bool _haveOffset = false;
	int64_t _offset = 0; //our clock minus the sender's, in master clocks
	bool _haveLast = false;
	uint32_t _lastTime = 0;
	uint64_t _senderUs = 0; //the sender's time, unwrapped
	bool _havePrev = false;
	uint64_t _prevDue = 0;
	int64_t _spare = INT64_MAX;
	uint64_t _windowStart = 0;
	uint32_t _late = 0;

	void Run();
	void Pick(uint64_t masterClock);
	uint64_t DueClock(const Record& r, uint64_t now);

public:
	WsLinkRoomDevice(uint16_t relayPort);
	~WsLinkRoomDevice();

	void OnSend(uint8_t value, uint64_t masterClock) override;
	bool Receive(uint8_t& value, uint64_t masterClock) override;
	bool KeepNewerOnOverrun() override { return _keepNewer; }
	bool IsLinked() override { return _started; }
};
