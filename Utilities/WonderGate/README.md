# wgcore

MobileWonderGate adapter emulation, used by the WonderSwan core's WonderGate device
(`Core/WS/WsWonderGateDevice.cpp`).

`wg.c` and `wg.h` are copied unchanged from the Swan Revival game server project
(`wondergate/core/` in WonderswanCartDumper), where the adapter protocol is worked out and tested
against each game. Update them by copying again rather than editing here. They are included in
MesenCE under its GPLv3 license. Protocol knowledge comes from trap15's wonderfence (MIT,
(C) 2014 Alex Marshall), the WSdev wiki, and a real adapter capture.
