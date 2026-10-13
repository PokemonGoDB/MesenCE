#include "pch.h"
#include "WS/WsSerial.h"
#include "WS/WsSerialDevice.h"
#include "WS/WsConsole.h"
#include "Shared/Emulator.h"
#include "Utilities/Serializer.h"

WsSerial::WsSerial(Emulator* emu, WsConsole* console)
{
	_emu = emu;
	_console = console;
}

uint8_t WsSerial::Read(uint16_t port)
{
	switch(port) {
		case 0xB1:
			UpdateState();
			//Reading the data port takes the byte: the receiver is free for the next one
			_state.HasReceiveData = false;
			return _state.ReceiveBuffer;

		case 0xB3:
			UpdateState();
			return (
				(_state.HasReceiveData ? 0x01 : 0) |
				(_state.ReceiveOverflow ? 0x02 : 0) |
				((!_state.Enabled || _state.HasSendData) ? 0 : 0x04) |
				(_state.HighSpeed ? 0x40 : 0) |
				(_state.Enabled ? 0x80 : 0));
	}

	return 0;
}

void WsSerial::Write(uint16_t port, uint8_t value)
{
	switch(port) {
		case 0xB1:
			UpdateState();
			_state.SendBuffer = value;
			_state.HasSendData = true;
			_state.SendClock = _console->GetMasterClock();
			break;

		case 0xB3:
			UpdateState();
			_state.Enabled = value & 0x80;
			_state.HighSpeed = value & 0x40;
			if(value & 0x20) {
				_state.ReceiveOverflow = false;
			}
			if(!_state.Enabled) {
				//Turning the port off resets the receiver: an unread byte is dropped
				_state.HasReceiveData = false;
			}
			break;
	}
}

void WsSerial::UpdateState()
{
	if(_state.HasSendData) {
		//At 9600 bauds/s, it takes 3200 master clocks to send 1 byte
		//At 38400 (high speed mode), it takes 800 master clocks
		uint64_t cyclesElapsed = _console->GetMasterClock() - _state.SendClock;
		if(cyclesElapsed > GetCyclesPerByte()) {
			_state.HasSendData = false;
			//The byte only reaches the other end if the port is on. Run-ahead frames are rolled
			//back, so they must not send anything outside the emulator.
			if(_device && _state.Enabled && !_emu->IsRunAheadFrame()) {
				_device->OnSend(_state.SendBuffer, _console->GetMasterClock());
			}
		}
	}

	UpdateReceive();
}

void WsSerial::UpdateReceive()
{
	//Nothing plugged in, or a run-ahead frame (bytes taken from the device would be lost on rollback)
	if(!_device || _emu->IsRunAheadFrame()) {
		return;
	}

	//One byte per byte time, like a real line
	uint64_t clock = _console->GetMasterClock();
	if(clock - _receiveClock < GetCyclesPerByte() || clock < _nextReceivePoll) {
		return;
	}

	uint8_t value;
	if(!_device->Receive(value, clock)) {
		_nextReceivePoll = clock + 256;
		return;
	}
	_receiveClock = clock;

	if(!_state.Enabled) {
		//The port is off: the byte is lost
	} else if(_state.HasReceiveData) {
		//The previous byte hasn't been read: this one is lost (status bit 1 until cleared with control bit 5)
		_state.ReceiveOverflow = true;
	} else {
		_state.ReceiveBuffer = value;
		_state.HasReceiveData = true;
	}
}

bool WsSerial::HasSendIrq()
{
	if(_state.HasSendData) {
		UpdateState();
	}
	return _state.Enabled && !_state.HasSendData;
}

bool WsSerial::HasReceiveIrq()
{
	UpdateReceive();
	return _state.Enabled && _state.HasReceiveData;
}

void WsSerial::Serialize(Serializer& s)
{
	SV(_state.Enabled);
	SV(_state.HighSpeed);
	SV(_state.HasReceiveData);
	SV(_state.HasSendData);
	SV(_state.SendBuffer);
	SV(_state.SendClock);
	SV(_state.ReceiveBuffer);
	SV(_state.ReceiveOverflow);
	SV(_receiveClock);
}
