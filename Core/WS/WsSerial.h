#pragma once
#include "pch.h"
#include "WS/WsTypes.h"
#include "Utilities/ISerializable.h"

class WsConsole;
class Emulator;
class IWsSerialDevice;

class WsSerial final : public ISerializable
{
private:
	WsSerialState _state = {};
	Emulator* _emu = nullptr;
	WsConsole* _console = nullptr;
	IWsSerialDevice* _device = nullptr;

	//Master clock when the last byte from the device started arriving
	uint64_t _receiveClock = 0;
	//Don't ask an idle device on every instruction (not saved: only affects polling)
	uint64_t _nextReceivePoll = 0;

	uint32_t GetCyclesPerByte() { return _state.HighSpeed ? 800 : 3200; }
	void UpdateState();
	void UpdateReceive();

public:
	WsSerial(Emulator* emu, WsConsole* console);

	//The device on the other end of the line (null = nothing plugged in)
	void SetDevice(IWsSerialDevice* device) { _device = device; }

	WsSerialState& GetState() { return _state; }

	uint8_t Read(uint16_t port);
	void Write(uint16_t port, uint8_t value);

	bool HasSendIrq();
	bool HasReceiveIrq();

	void Serialize(Serializer& s) override;
};