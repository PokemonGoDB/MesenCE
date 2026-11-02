#pragma once
#include "pch.h"
#include <cstdlib>

//Something plugged into the serial line of the WonderSwan's EXT port: a MobileWonderGate
//adapter, a link cable to another console, a PC. WsSerial calls these on the emulation thread,
//so implementations must not block.
class IWsSerialDevice
{
public:
	virtual ~IWsSerialDevice() = default;

	//The console finished sending a byte. masterClock is the WonderSwan's master clock
	//(3,072,000 per second), for devices that keep time.
	virtual void OnSend(uint8_t value, uint64_t masterClock) = 0;

	//The next byte the device is sending to the console, if one is ready.
	//Called at most once per byte time (the line's baud rate). Once returned, the byte is on the
	//line: if the console can't take it (port off, previous byte unread) it is lost, as on hardware.
	virtual bool Receive(uint8_t& value, uint64_t masterClock) = 0;

	//When a byte arrives before the console read the last one, keep the newer byte (some link games need that;
	//real hardware likely does it) rather than the older
	virtual bool KeepNewerOnOverrun() { return false; }

	//Linked to another console right now: save states, rewind and run-ahead can't take the other end back in
	//time, so the emulator refuses them meanwhile
	virtual bool IsLinked() { return false; }
};

//An environment variable, or "" (portable: MSVC treats getenv as unsafe)
inline string WsReadEnv(const char* name)
{
#ifdef _WIN32
	char* buffer = nullptr;
	size_t length = 0;
	string value;
	if(_dupenv_s(&buffer, &length, name) == 0 && buffer) {
		value = buffer;
		free(buffer);
	}
	return value;
#else
	const char* value = std::getenv(name);
	return value ? value : "";
#endif
}
